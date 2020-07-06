#include "input.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <array>
#include <exception>
#include <locale>
#include <memory>
#include <set>
#include <utility>

#include "action.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "cursesdef.h"
#include "debug.h"
#include "filesystem.h"
#include "game.h"
#include "help.h"
#include "json.h"
#include "optional.h"
#include "options.h"
#include "output.h"
#include "path_info.h"
#include "popup.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "ui_manager.h"
#include "color.h"
#include "point.h"
#include "sdltiles.h"

using std::min; // from <algorithm>
using std::max;

static const std::string default_context_id( "default" );

template <class T1, class T2>
struct ContainsPredicate {
    const T1 &container;

    ContainsPredicate( const T1 &container ) : container( container ) { }

    // Operator overload required to leverage std functional interface.
    bool operator()( T2 c ) {
        return std::find( container.begin(), container.end(), c ) != container.end();
    }
};

static int str_to_int( const std::string &number )
{
    // ensure user's locale doesn't interfere with number format
    std::istringstream buffer( number );
    buffer.imbue( std::locale::classic() );
    int result;
    buffer >> result;
    return result;
}

static std::string int_to_str( int number )
{
    // ensure user's locale doesn't interfere with number format
    std::ostringstream buffer;
    buffer.imbue( std::locale::classic() );
    buffer << number;
    return buffer.str();
}

bool is_mouse_enabled()
{
#if defined(_WIN32) && !defined(TILES)
    return false;
#else
    return true;
#endif
}

static bool is_keycode_mode_supported()
{
#if defined(TILES) and !defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

//helper function for those have problem inputting certain characters.
std::string get_input_string_from_file( const std::string &fname )
{
    std::string ret;
    read_from_file_optional( fname, [&ret]( std::istream & fin ) {
        getline( fin, ret );
        //remove utf8 bmm
        if( !ret.empty() && static_cast<unsigned char>( ret[0] ) == 0xef ) {
            ret.erase( 0, 3 );
        }
        while( !ret.empty() && ( ret.back() == '\r' ||  ret.back() == '\n' ) ) {
            ret.erase( ret.size() - 1, 1 );
        }
    } );
    return ret;
}

input_event::input_event( const std::set<keymod_t> &mod, const int s, const input_event_t t )
    : type( t ), modifiers( mod ), edit_refresh( false )
{
    sequence.emplace_back( s );
#if defined(__ANDROID__)
    shortcut_last_used_action_counter = 0;
#endif
}

int input_event::get_first_input() const
{
    if( sequence.empty() ) {
        return UNKNOWN_UNICODE;
    }

    return sequence[0];
}

input_manager inp_mngr;

void input_manager::init()
{
    std::map<char, action_id> keymap;
    std::string keymap_file_loaded_from;
    std::set<action_id> unbound_keymap;
    load_keyboard_settings( keymap, keymap_file_loaded_from, unbound_keymap );
    init_keycode_mapping();
    reset_timeout();

    try {
        load( PATH_INFO::keybindings(), false );
    } catch( const JsonError &err ) {
        throw std::runtime_error( PATH_INFO::keybindings() + ": " + err.what() );
    }
    try {
        load( PATH_INFO::keybindings_vehicle(), false );
    } catch( const JsonError &err ) {
        throw std::runtime_error( PATH_INFO::keybindings_vehicle() + ": " + err.what() );
    }
    try {
        load( PATH_INFO::user_keybindings(), true );
    } catch( const JsonError &err ) {
        throw std::runtime_error( PATH_INFO::user_keybindings() + ": " + err.what() );
    }

    if( keymap_file_loaded_from.empty() || ( keymap.empty() && unbound_keymap.empty() ) ) {
        // No keymap file was loaded, or the file has no mappings and no unmappings,
        // we can skip the remaining part of the function, especially the save function
        return;
    }
    t_actions &actions = action_contexts["DEFAULTMODE"];
    std::set<action_id> touched;
    for( const std::pair<const char, action_id> &a : keymap ) {
        const std::string action_id = action_ident( a.second );
        // Put the binding from keymap either into the global context
        // (if an action with that ident already exists there - think movement keys)
        // or otherwise to the DEFAULTMODE context.
        std::string context = "DEFAULTMODE";
        if( action_contexts[default_context_id].count( action_id ) > 0 ) {
            context = default_context_id;
        } else if( touched.count( a.second ) == 0 ) {
            // Note: movement keys are somehow special as the default in keymap
            // does not contain the arrow keys, so we don't clear existing keybindings
            // for them.
            // If the keymap contains a binding for this action, erase all the
            // previously (default!) existing bindings, to only keep the bindings,
            // the user is used to
            action_contexts[action_id].clear();
            touched.insert( a.second );
        }
        add_input_for_action( action_id, context, input_event( a.first, input_event_t::keyboard_char ) );
    }
    // Unmap actions that are explicitly not mapped
    for( const auto &elem : unbound_keymap ) {
        const std::string action_id = action_ident( elem );
        actions[action_id].input_events.clear();
    }
    // Imported old bindings from old keymap file, save those to the new
    // keybindings.json file.
    try {
        save();
    } catch( std::exception &err ) {
        debugmsg( "Could not write imported keybindings: %s", err.what() );
        return;
    }
    // Finally if we did import a file, and saved it to the new keybindings
    // file, delete the old keymap file to prevent re-importing it.
    if( !keymap_file_loaded_from.empty() ) {
        remove_file( keymap_file_loaded_from );
    }
}

static constexpr int current_keybinding_version = 1;

void input_manager::load( const std::string &file_name, bool is_user_preferences )
{
    std::ifstream data_file( file_name.c_str(), std::ifstream::in | std::ifstream::binary );

    if( !data_file.good() ) {
        // Only throw if this is the first file to load, that file _must_ exist,
        // otherwise the keybindings can not be read at all.
        if( action_contexts.empty() ) {
            throw std::runtime_error( std::string( "Could not read " ) + file_name );
        }
        return;
    }

    JsonIn jsin( data_file );

    //Crawl through once and create an entry for every definition
    jsin.start_array();
    while( !jsin.end_array() ) {
        // JSON object representing the action
        JsonObject action = jsin.get_object();

        int version = current_keybinding_version;
        if( is_user_preferences ) {
            // if there isn't a "version" value it means the object was written before
            // introduction of keybinding version, which is denoted by version 0.
            version = action.get_int( "version", 0 );
        }

        const std::string type = action.get_string( "type", "keybinding" );
        if( type != "keybinding" ) {
            debugmsg( "Only objects of type 'keybinding' (not %s) should appear in the "
                      "keybindings file '%s'", type, file_name );
            continue;
        }

        const std::string action_id = action.get_string( "id" );
        const std::string context = action.get_string( "category", default_context_id );
        t_actions &actions = action_contexts[context];
        if( !is_user_preferences && action.has_member( "name" ) ) {
            // Action names are not user preferences. Some experimental builds
            // post-0.A had written action names into the user preferences
            // config file. Any names that exist in user preferences will be
            // ignored.
            action.read( "name", actions[action_id].name );
        }

        t_input_event_list events;
        for( const JsonObject keybinding : action.get_array( "bindings" ) ) {
            std::string input_method = keybinding.get_string( "input_method" );
            std::vector<input_event> new_events( 1 );
            if( input_method == "keyboard_any" ) {
                new_events.resize( 2 );
                new_events[0].type = input_event_t::keyboard_char;
                new_events[1].type = input_event_t::keyboard_code;
            } else if( input_method == "keyboard_char" || input_method == "keyboard" ) {
                new_events[0].type = input_event_t::keyboard_char;
            } else if( input_method == "keyboard_code" ) {
                new_events[0].type = input_event_t::keyboard_code;
            } else if( input_method == "gamepad" ) {
                new_events[0].type = input_event_t::gamepad;
            } else if( input_method == "mouse" ) {
                new_events[0].type = input_event_t::mouse;
            } else {
                keybinding.throw_error( "unknown input_method", "input_method" );
            }

            if( keybinding.has_member( "mod" ) ) {
                for( const JsonValue &val : keybinding.get_array( "mod" ) ) {
                    const std::string str = val;
                    keymod_t mod = keymod_t::ctrl;
                    if( str == "ctrl" ) {
                        mod = keymod_t::ctrl;
                    } else if( str == "alt" ) {
                        mod = keymod_t::alt;
                    } else if( str == "shift" ) {
                        mod = keymod_t::shift;
                    } else {
                        val.throw_error( "unknown modifier name" );
                    }
                    for( input_event &new_event : new_events ) {
                        new_event.modifiers.emplace( mod );
                    }
                }
            }

            if( keybinding.has_array( "key" ) ) {
                for( const std::string line : keybinding.get_array( "key" ) ) {
                    for( input_event &new_event : new_events ) {
                        new_event.sequence.push_back( get_keycode( new_event.type, line ) );
                    }
                }
            } else { // assume string if not array, and throw if not string
                for( input_event &new_event : new_events ) {
                    new_event.sequence.push_back(
                        get_keycode( new_event.type, keybinding.get_string( "key" ) )
                    );
                }
            }

            events.insert( events.end(), new_events.begin(), new_events.end() );
        }

        // An invariant of this class is that user-created, local keybindings
        // with an empty set of input_events do not exist in the
        // action_contexts map. In prior versions of this class, this was not
        // true, so users of experimental builds post-0.A will have empty
        // local keybindings saved in their keybindings.json config.
        //
        // To be backwards compatible with keybindings.json from prior
        // experimental builds, we will detect user-created, local keybindings
        // with empty input_events and disregard them. When keybindings are
        // later saved, these remnants won't be saved.
        if( !is_user_preferences ||
            !events.empty() ||
            context == default_context_id ||
            actions.count( action_id ) > 0 ) {
            // In case this is the second file containing user preferences,
            // this replaces the default bindings with the user's preferences.
            action_attributes &attributes = actions[action_id];
            if( is_user_preferences && version == 0 ) {
                // version 0 means the keybinding was written prior to the division
                // of `input_event_t::keyboard_char` and `input_event_t::keyboard_code`,
                // so we copy any `input_event_t::keyboard_code` event from the default
                // keybindings to be compatible with old user keybinding files.
                for( const input_event &evt : attributes.input_events ) {
                    if( evt.type == input_event_t::keyboard_code ) {
                        events.emplace_back( evt );
                    }
                }
            }
            attributes.input_events = events;
            if( action.has_member( "is_user_created" ) ) {
                attributes.is_user_created = action.get_bool( "is_user_created" );
            }
        }
    }
}

void input_manager::save()
{
    write_to_file( PATH_INFO::user_keybindings(), [&]( std::ostream & data_file ) {
        JsonOut jsout( data_file, true );

        jsout.start_array();
        for( const auto &a : action_contexts ) {
            const t_actions &actions = a.second;
            for( const auto &action : actions ) {
                const t_input_event_list &events = action.second.input_events;
                jsout.start_object();

                jsout.member( "id", action.first );
                jsout.member( "version", current_keybinding_version );
                jsout.member( "category", a.first );
                bool is_user_created = action.second.is_user_created;
                if( is_user_created ) {
                    jsout.member( "is_user_created", is_user_created );
                }

                jsout.member( "bindings" );
                jsout.start_array();
                for( const auto &event : events ) {
                    jsout.start_object();
                    switch( event.type ) {
                        case input_event_t::keyboard_char:
                            jsout.member( "input_method", "keyboard_char" );
                            break;
                        case input_event_t::keyboard_code:
                            jsout.member( "input_method", "keyboard_code" );
                            break;
                        case input_event_t::gamepad:
                            jsout.member( "input_method", "gamepad" );
                            break;
                        case input_event_t::mouse:
                            jsout.member( "input_method", "mouse" );
                            break;
                        default:
                            throw std::runtime_error( "unknown input_event_t" );
                    }

                    jsout.member( "mod" );
                    jsout.start_array();
                    for( const keymod_t mod : event.modifiers ) {
                        switch( mod ) {
                            case keymod_t::ctrl:
                                jsout.write( "ctrl" );
                                break;
                            case keymod_t::alt:
                                jsout.write( "alt" );
                                break;
                            case keymod_t::shift:
                                jsout.write( "shift" );
                                break;
                            default:
                                throw std::runtime_error( "unknown keymod_t" );
                        }
                    }
                    jsout.end_array();

                    jsout.member( "key" );
                    jsout.start_array();
                    for( size_t i = 0; i < event.sequence.size(); i++ ) {
                        jsout.write( get_keyname( event.sequence[i], event.type, true ) );
                    }
                    jsout.end_array();
                    jsout.end_object();
                }
                jsout.end_array();

                jsout.end_object();
            }
        }
        jsout.end_array();
    }, _( "key bindings configuration" ) );
}

void input_manager::add_keyboard_char_keycode_pair( int ch, const std::string &name )
{
    keyboard_char_keycode_to_keyname[ch] = name;
    keyboard_char_keyname_to_keycode[name] = ch;
}

void input_manager::add_keyboard_code_keycode_pair( const int ch, const std::string &name )
{
    keyboard_code_keycode_to_keyname[ch] = name;
    keyboard_code_keyname_to_keycode[name] = ch;
}

void input_manager::add_gamepad_keycode_pair( int ch, const std::string &name )
{
    gamepad_keycode_to_keyname[ch] = name;
    gamepad_keyname_to_keycode[name] = ch;
}

void input_manager::add_mouse_keycode_pair( const int ch, const std::string &name )
{
    mouse_keycode_to_keyname[ch] = name;
    mouse_keyname_to_keycode[name] = ch;
}

constexpr int char_key_beg = ' ';
constexpr int char_key_end = '~';

void input_manager::init_keycode_mapping()
{
    // Between space and tilde, all keys more or less map
    // to themselves(see ASCII table)
    for( char c = char_key_beg; c <= char_key_end; c++ ) {
        std::string name( 1, c );
        add_keyboard_char_keycode_pair( c, name );
        add_keyboard_code_keycode_pair( c, name );
    }

    add_keyboard_char_keycode_pair( '\t',          translate_marker_context( "key name", "TAB" ) );
    add_keyboard_char_keycode_pair( KEY_BTAB,      translate_marker_context( "key name", "BACKTAB" ) );
    add_keyboard_char_keycode_pair( ' ',           translate_marker_context( "key name", "SPACE" ) );
    add_keyboard_char_keycode_pair( KEY_UP,        translate_marker_context( "key name", "UP" ) );
    add_keyboard_char_keycode_pair( KEY_DOWN,      translate_marker_context( "key name", "DOWN" ) );
    add_keyboard_char_keycode_pair( KEY_LEFT,      translate_marker_context( "key name", "LEFT" ) );
    add_keyboard_char_keycode_pair( KEY_RIGHT,     translate_marker_context( "key name", "RIGHT" ) );
    add_keyboard_char_keycode_pair( KEY_NPAGE,     translate_marker_context( "key name", "NPAGE" ) );
    add_keyboard_char_keycode_pair( KEY_PPAGE,     translate_marker_context( "key name", "PPAGE" ) );
    add_keyboard_char_keycode_pair( KEY_ESCAPE,    translate_marker_context( "key name", "ESC" ) );
    add_keyboard_char_keycode_pair( KEY_BACKSPACE,
                                    translate_marker_context( "key name", "BACKSPACE" ) );
    add_keyboard_char_keycode_pair( KEY_HOME,      translate_marker_context( "key name", "HOME" ) );
    add_keyboard_char_keycode_pair( KEY_BREAK,     translate_marker_context( "key name", "BREAK" ) );
    add_keyboard_char_keycode_pair( KEY_END,       translate_marker_context( "key name", "END" ) );
    add_keyboard_char_keycode_pair( '\n',          translate_marker_context( "key name", "RETURN" ) );

    // function keys, as defined by ncurses
    for( int i = F_KEY_NUM_BEG; i <= F_KEY_NUM_END; i++ ) {
        // not marked for translation here, but specially handled in get_keyname so
        // it gets properly translated.
        add_keyboard_char_keycode_pair( KEY_F( i ), string_format( "F%d", i ) );
    }

    static const std::vector<std::pair<int, std::string>> keyboard_code_keycode_pair = {
        { keycode::backspace, translate_marker_context( "key name", "BACKSPACE" ) },
        { keycode::tab,       translate_marker_context( "key name", "TAB" ) },
        { keycode::return_,   translate_marker_context( "key name", "RETURN" ) },
        { keycode::escape,    translate_marker_context( "key name", "ESC" ) },
        { keycode::space,     translate_marker_context( "key name", "SPACE" ) },
        { keycode::f1,        translate_marker_context( "key name", "F1" ) },
        { keycode::f2,        translate_marker_context( "key name", "F2" ) },
        { keycode::f3,        translate_marker_context( "key name", "F3" ) },
        { keycode::f4,        translate_marker_context( "key name", "F4" ) },
        { keycode::f5,        translate_marker_context( "key name", "F5" ) },
        { keycode::f6,        translate_marker_context( "key name", "F6" ) },
        { keycode::f7,        translate_marker_context( "key name", "F7" ) },
        { keycode::f8,        translate_marker_context( "key name", "F8" ) },
        { keycode::f9,        translate_marker_context( "key name", "F9" ) },
        { keycode::f10,       translate_marker_context( "key name", "F10" ) },
        { keycode::f11,       translate_marker_context( "key name", "F11" ) },
        { keycode::f12,       translate_marker_context( "key name", "F12" ) },
        { keycode::ppage,     translate_marker_context( "key name", "PPAGE" ) },
        { keycode::home,      translate_marker_context( "key name", "HOME" ) },
        { keycode::end,       translate_marker_context( "key name", "END" ) },
        { keycode::npage,     translate_marker_context( "key name", "NPAGE" ) },
        { keycode::right,     translate_marker_context( "key name", "RIGHT" ) },
        { keycode::left,      translate_marker_context( "key name", "LEFT" ) },
        { keycode::down,      translate_marker_context( "key name", "DOWN" ) },
        { keycode::up,        translate_marker_context( "key name", "UP" ) },
        { keycode::kp_divide, translate_marker_context( "key name", "KEYPAD_DIVIDE" ) },
        { keycode::kp_multiply, translate_marker_context( "key name", "KEYPAD_MULTIPLY" ) },
        { keycode::kp_minus,  translate_marker_context( "key name", "KEYPAD_MINUS" ) },
        { keycode::kp_plus,   translate_marker_context( "key name", "KEYPAD_PLUS" ) },
        { keycode::kp_enter,  translate_marker_context( "key name", "KEYPAD_ENTER" ) },
        { keycode::kp_1,      translate_marker_context( "key name", "KEYPAD_1" ) },
        { keycode::kp_2,      translate_marker_context( "key name", "KEYPAD_2" ) },
        { keycode::kp_3,      translate_marker_context( "key name", "KEYPAD_3" ) },
        { keycode::kp_4,      translate_marker_context( "key name", "KEYPAD_4" ) },
        { keycode::kp_5,      translate_marker_context( "key name", "KEYPAD_5" ) },
        { keycode::kp_6,      translate_marker_context( "key name", "KEYPAD_6" ) },
        { keycode::kp_7,      translate_marker_context( "key name", "KEYPAD_7" ) },
        { keycode::kp_8,      translate_marker_context( "key name", "KEYPAD_8" ) },
        { keycode::kp_9,      translate_marker_context( "key name", "KEYPAD_9" ) },
        { keycode::kp_0,      translate_marker_context( "key name", "KEYPAD_0" ) },
        { keycode::kp_period, translate_marker_context( "key name", "KEYPAD_PERIOD" ) },
        { keycode::f13,       translate_marker_context( "key name", "F13" ) },
        { keycode::f14,       translate_marker_context( "key name", "F14" ) },
        { keycode::f15,       translate_marker_context( "key name", "F15" ) },
        { keycode::f16,       translate_marker_context( "key name", "F16" ) },
        { keycode::f17,       translate_marker_context( "key name", "F17" ) },
        { keycode::f18,       translate_marker_context( "key name", "F18" ) },
        { keycode::f19,       translate_marker_context( "key name", "F19" ) },
        { keycode::f20,       translate_marker_context( "key name", "F20" ) },
        { keycode::f21,       translate_marker_context( "key name", "F21" ) },
        { keycode::f22,       translate_marker_context( "key name", "F22" ) },
        { keycode::f23,       translate_marker_context( "key name", "F23" ) },
        { keycode::f24,       translate_marker_context( "key name", "F24" ) },
    };
    for( const auto &v : keyboard_code_keycode_pair ) {
        add_keyboard_code_keycode_pair( v.first, v.second );
    }

    add_gamepad_keycode_pair( JOY_LEFT,      translate_marker_context( "key name", "JOY_LEFT" ) );
    add_gamepad_keycode_pair( JOY_RIGHT,     translate_marker_context( "key name", "JOY_RIGHT" ) );
    add_gamepad_keycode_pair( JOY_UP,        translate_marker_context( "key name", "JOY_UP" ) );
    add_gamepad_keycode_pair( JOY_DOWN,      translate_marker_context( "key name", "JOY_DOWN" ) );
    add_gamepad_keycode_pair( JOY_LEFTUP,    translate_marker_context( "key name", "JOY_LEFTUP" ) );
    add_gamepad_keycode_pair( JOY_LEFTDOWN,  translate_marker_context( "key name", "JOY_LEFTDOWN" ) );
    add_gamepad_keycode_pair( JOY_RIGHTUP,   translate_marker_context( "key name", "JOY_RIGHTUP" ) );
    add_gamepad_keycode_pair( JOY_RIGHTDOWN, translate_marker_context( "key name", "JOY_RIGHTDOWN" ) );

    add_gamepad_keycode_pair( JOY_0,         translate_marker_context( "key name", "JOY_0" ) );
    add_gamepad_keycode_pair( JOY_1,         translate_marker_context( "key name", "JOY_1" ) );
    add_gamepad_keycode_pair( JOY_2,         translate_marker_context( "key name", "JOY_2" ) );
    add_gamepad_keycode_pair( JOY_3,         translate_marker_context( "key name", "JOY_3" ) );
    add_gamepad_keycode_pair( JOY_4,         translate_marker_context( "key name", "JOY_4" ) );
    add_gamepad_keycode_pair( JOY_5,         translate_marker_context( "key name", "JOY_5" ) );
    add_gamepad_keycode_pair( JOY_6,         translate_marker_context( "key name", "JOY_6" ) );
    add_gamepad_keycode_pair( JOY_7,         translate_marker_context( "key name", "JOY_7" ) );

    add_mouse_keycode_pair( MOUSE_BUTTON_LEFT,  translate_marker_context( "key name", "MOUSE_LEFT" ) );
    add_mouse_keycode_pair( MOUSE_BUTTON_RIGHT, translate_marker_context( "key name", "MOUSE_RIGHT" ) );
    add_mouse_keycode_pair( SCROLLWHEEL_UP,     translate_marker_context( "key name", "SCROLL_UP" ) );
    add_mouse_keycode_pair( SCROLLWHEEL_DOWN,   translate_marker_context( "key name", "SCROLL_DOWN" ) );
    add_mouse_keycode_pair( MOUSE_MOVE,         translate_marker_context( "key name", "MOUSE_MOVE" ) );
}

int input_manager::get_keycode( const input_event_t inp_type, const std::string &name ) const
{
    const t_name_to_key_map *map = nullptr;
    switch( inp_type ) {
        default:
            break;
        case input_event_t::keyboard_char:
            map = &keyboard_char_keyname_to_keycode;
            break;
        case input_event_t::keyboard_code:
            map = &keyboard_code_keyname_to_keycode;
            break;
        case input_event_t::gamepad:
            map = &gamepad_keyname_to_keycode;
            break;
        case input_event_t::mouse:
            map = &mouse_keyname_to_keycode;
            break;
    }
    if( map ) {
        const auto it = map->find( name );
        if( it != map->end() ) {
            return it->second;
        }
    }
    // Not found in map, try to parse as int
    if( name.compare( 0, 8, "UNKNOWN_" ) == 0 ) {
        return str_to_int( name.substr( 8 ) );
    }
    return 0;
}

std::string input_manager::get_keyname( int ch, input_event_t inp_type, bool portable ) const
{
    const t_key_to_name_map *map = nullptr;
    switch( inp_type ) {
        default:
            break;
        case input_event_t::keyboard_char:
            map = &keyboard_char_keycode_to_keyname;
            break;
        case input_event_t::keyboard_code:
            map = &keyboard_code_keycode_to_keyname;
            break;
        case input_event_t::gamepad:
            map = &gamepad_keycode_to_keyname;
            break;
        case input_event_t::mouse:
            map = &mouse_keycode_to_keyname;
            break;
    }
    if( map ) {
        const auto it = map->find( ch );
        if( it != map->end() ) {
            switch( inp_type ) {
                case input_event_t::keyboard_char:
                    if( IS_F_KEY( ch ) ) {
                        // special case it since F<num> key names are generated using loop
                        // and not marked individually for translation
                        if( portable ) {
                            return it->second;
                        } else {
                            return string_format( pgettext( "function key name", "F%d" ), F_KEY_NUM( ch ) );
                        }
                    } else if( ch >= char_key_beg && ch <= char_key_end && ch != ' ' ) {
                        // character keys except space need no translation
                        return it->second;
                    }
                    break;
                case input_event_t::keyboard_code:
                    if( ch >= char_key_beg && ch < char_key_end && ch != ' ' ) {
                        // character keys except space need no translation
                        return it->second;
                    }
                    break;
                default:
                    break;
            }
            return portable ? it->second : pgettext( "key name", it->second.c_str() );
        }
    }
    if( portable ) {
        return std::string( "UNKNOWN_" ) + int_to_str( ch );
    } else {
        return string_format( _( "unknown key %ld" ), ch );
    }
}

const std::vector<input_event> &input_manager::get_input_for_action( const std::string
        &action_descriptor, const std::string &context, bool *overwrites_default )
{
    const action_attributes &attributes = get_action_attributes( action_descriptor, context,
                                          overwrites_default );
    return attributes.input_events;
}

int input_manager::get_first_char_for_action( const std::string &action_descriptor,
        const std::string &context )
{
    std::vector<input_event> input_events = get_input_for_action( action_descriptor, context );
    return input_events.empty() ? 0 : input_events[0].get_first_input();
}

const action_attributes &input_manager::get_action_attributes(
    const std::string &action_id,
    const std::string &context,
    bool *overwrites_default )
{

    if( context != default_context_id ) {
        // Check if the action exists in the provided context
        const t_action_contexts::const_iterator action_context = action_contexts.find( context );
        if( action_context != action_contexts.end() ) {
            const t_actions::const_iterator action = action_context->second.find( action_id );
            if( action != action_context->second.end() ) {
                if( overwrites_default ) {
                    *overwrites_default = true;
                }

                return action->second;
            }
        }
    }

    // If not, we use the default binding.
    if( overwrites_default ) {
        *overwrites_default = false;
    }

    t_actions &default_action_context = action_contexts[default_context_id];
    const t_actions::const_iterator default_action = default_action_context.find( action_id );
    if( default_action == default_action_context.end() ) {
        // A new action is created in the event that the requested action is
        // not in the keybindings configuration e.g. the entry is missing.
        default_action_context[action_id].name = get_default_action_name( action_id );
    }

    return default_action_context[action_id];
}

translation input_manager::get_default_action_name( const std::string &action_id ) const
{
    const t_action_contexts::const_iterator default_action_context = action_contexts.find(
                default_context_id );
    if( default_action_context == action_contexts.end() ) {
        return no_translation( action_id );
    }

    const t_actions::const_iterator default_action = default_action_context->second.find( action_id );
    if( default_action != default_action_context->second.end() ) {
        return default_action->second.name;
    } else {
        return no_translation( action_id );
    }
}

input_manager::t_input_event_list &input_manager::get_or_create_event_list(
    const std::string &action_descriptor, const std::string &context )
{
    // A new context is created in the event that the user creates a local
    // keymapping in a context that doesn't yet exist e.g. a context without
    // any pre-existing keybindings.
    t_actions &actions = action_contexts[context];

    // A new action is created in the event that the user creates a local
    // keymapping that masks a global one.
    if( actions.find( action_descriptor ) == actions.end() ) {
        action_attributes &attributes = actions[action_descriptor];
        attributes.name = get_default_action_name( action_descriptor );
        attributes.is_user_created = true;
    }

    return actions[action_descriptor].input_events;
}

void input_manager::remove_input_for_action(
    const std::string &action_descriptor, const std::string &context )
{
    const t_action_contexts::iterator action_context = action_contexts.find( context );
    if( action_context != action_contexts.end() ) {
        t_actions &actions = action_context->second;
        t_actions::iterator action = actions.find( action_descriptor );
        if( action != actions.end() ) {
            if( action->second.is_user_created ) {
                // Since this is a user created hotkey, remove it so that the
                // user will fallback to the hotkey in the default context.
                actions.erase( action );
            } else {
                // If a context no longer has any keybindings remaining for an action but
                // there's an attempt to remove bindings anyway, presumably the user wants
                // to fully remove the binding from that context.
                if( action->second.input_events.empty() ) {
                    actions.erase( action );
                } else {
                    action->second.input_events.clear();
                }
            }
        }
    }
}

void input_manager::add_input_for_action(
    const std::string &action_descriptor, const std::string &context, const input_event &event )
{
    t_input_event_list &events = get_or_create_event_list( action_descriptor, context );
    for( auto &events_a : events ) {
        if( events_a == event ) {
            return;
        }
    }
    events.push_back( event );
}

bool input_context::action_uses_input( const std::string &action_id,
                                       const input_event &event ) const
{
    const auto &events = inp_mngr.get_action_attributes( action_id, category ).input_events;
    return std::find( events.begin(), events.end(), event ) != events.end();
}

std::string input_context::get_conflicts( const input_event &event ) const
{
    return enumerate_as_string( registered_actions.begin(), registered_actions.end(),
    [ this, &event ]( const std::string & action ) {
        return action_uses_input( action, event ) ? get_action_name( action ) : std::string();
    } );
}

void input_context::clear_conflicting_keybindings( const input_event &event )
{
    // The default context is always included to cover cases where the same
    // keybinding exists for the same action in both the global and local
    // contexts.
    input_manager::t_actions &default_actions = inp_mngr.action_contexts[default_context_id];
    input_manager::t_actions &category_actions = inp_mngr.action_contexts[category];

    for( const auto &registered_action : registered_actions ) {
        input_manager::t_actions::iterator default_action = default_actions.find( registered_action );
        input_manager::t_actions::iterator category_action = category_actions.find( registered_action );
        if( default_action != default_actions.end() ) {
            std::vector<input_event> &events = default_action->second.input_events;
            events.erase( std::remove( events.begin(), events.end(), event ), events.end() );
        }
        if( category_action != category_actions.end() ) {
            std::vector<input_event> &events = category_action->second.input_events;
            events.erase( std::remove( events.begin(), events.end(), event ), events.end() );
        }
    }
}

const std::string CATA_ERROR = "ERROR";
const std::string ANY_INPUT = "ANY_INPUT";
const std::string HELP_KEYBINDINGS = "HELP_KEYBINDINGS";
const std::string COORDINATE = "COORDINATE";
const std::string TIMEOUT = "TIMEOUT";

const std::string &input_context::input_to_action( const input_event &inp ) const
{
    for( auto &elem : registered_actions ) {
        const std::string &action = elem;
        const std::vector<input_event> &check_inp = inp_mngr.get_input_for_action( action, category );

        // Does this action have our queried input event in its keybindings?
        for( auto &check_inp_i : check_inp ) {
            if( check_inp_i == inp ) {
                return action;
            }
        }
    }
    return CATA_ERROR;
}

#if defined(__ANDROID__)
std::list<input_context *> input_context::input_context_stack;

void input_context::register_manual_key( manual_key mk )
{
    // Prevent duplicates
    for( const manual_key &manual_key : registered_manual_keys )
        if( manual_key.key == mk.key ) {
            return;
        }

    registered_manual_keys.push_back( mk );
}

void input_context::register_manual_key( int key, const std::string text )
{
    // Prevent duplicates
    for( const manual_key &manual_key : registered_manual_keys )
        if( manual_key.key == key ) {
            return;
        }

    registered_manual_keys.push_back( manual_key( key, text ) );
}
#endif

void input_context::register_action( const std::string &action_descriptor )
{
    register_action( action_descriptor, translation() );
}

void input_context::register_action( const std::string &action_descriptor, const translation &name )
{
    if( action_descriptor == "ANY_INPUT" ) {
        registered_any_input = true;
    } else if( action_descriptor == "COORDINATE" ) {
        handling_coordinate_input = true;
    }

    registered_actions.push_back( action_descriptor );
    if( !name.empty() ) {
        action_name_overrides[action_descriptor] = name;
    }
}

std::vector<char> input_context::keys_bound_to( const std::string &action_descriptor,
        const bool restrict_to_printable ) const
{
    std::vector<char> result;
    const std::vector<input_event> &events = inp_mngr.get_input_for_action( action_descriptor,
            category );
    for( const auto &events_event : events ) {
        // Ignore multi-key input and non-keyboard input
        // TODO: fix for Unicode.
        if( events_event.type == input_event_t::keyboard_char && events_event.sequence.size() == 1 ) {
            if( !restrict_to_printable || ( events_event.sequence.front() < 0xFF &&
                                            isprint( events_event.sequence.front() ) ) ) {
                result.push_back( static_cast<char>( events_event.sequence.front() ) );
            }
        }
    }
    return result;
}

std::string input_context::key_bound_to( const std::string &action_descriptor, const size_t index,
        const bool restrict_to_printable ) const
{
    const auto bound_keys = keys_bound_to( action_descriptor, restrict_to_printable );
    return bound_keys.size() > index ? std::string( 1, bound_keys[index] ) : "";
}

std::string input_context::get_available_single_char_hotkeys( std::string requested_keys )
{
    for( const auto &registered_action : registered_actions ) {

        const std::vector<input_event> &events = inp_mngr.get_input_for_action( registered_action,
                category );
        for( const auto &events_event : events ) {
            // Only consider keyboard events without modifiers
            if( events_event.type == input_event_t::keyboard_char && events_event.modifiers.empty() ) {
                requested_keys.erase( std::remove_if( requested_keys.begin(), requested_keys.end(),
                                                      ContainsPredicate<std::vector<int>, char>(
                                                              events_event.sequence ) ),
                                      requested_keys.end() );
            }
        }
    }

    return requested_keys;
}

const input_context::input_event_filter input_context::disallow_lower_case =
[]( const input_event &evt ) -> bool {
    return evt.type != input_event_t::keyboard_char ||
    // std::lower from <cctype> is undefined outside unsigned char range
    // and std::lower from <locale> may throw bad_cast for some locales
    evt.get_first_input() < 'a' || evt.get_first_input() > 'z';
};

const input_context::input_event_filter input_context::allow_all_keys =
[]( const input_event & ) -> bool {
    return true;
};

static const std::vector<std::pair<keymod_t, translation>> keymod_desc = {
    { keymod_t::ctrl,  to_translation( "key modifier", "CTRL-" ) },
    { keymod_t::alt,   to_translation( "key modifier", "ALT-" ) },
    { keymod_t::shift, to_translation( "key modifier", "SHIFT-" ) },
};

std::string input_context::get_desc( const std::string &action_descriptor,
                                     const unsigned int max_limit,
                                     const input_context::input_event_filter &evt_filter ) const
{
    if( action_descriptor == "ANY_INPUT" ) {
        return "(*)"; // * for wildcard
    }

    bool is_local = false;
    const std::vector<input_event> &events = inp_mngr.get_input_for_action( action_descriptor,
            category, &is_local );

    if( events.empty() ) {
        return is_local ? _( "Unbound locally!" ) : _( "Unbound globally!" );
    }

    std::vector<input_event> inputs_to_show;
    for( auto &events_i : events ) {
        const input_event &event = events_i;

        if( is_event_type_enabled( event.type ) && evt_filter( event ) ) {
            inputs_to_show.push_back( event );
        }

        if( max_limit > 0 && inputs_to_show.size() == max_limit ) {
            break;
        }
    }

    if( inputs_to_show.empty() ) {
        return pgettext( "keybinding", "Disabled" );
    }

    std::string rval;
    for( size_t i = 0; i < inputs_to_show.size(); ++i ) {
        // test in fixed order to generate consistent description
        for( const auto &v : keymod_desc ) {
            if( inputs_to_show[i].modifiers.count( v.first ) ) {
                rval += v.second.translated();
            }
        }
        for( size_t j = 0; j < inputs_to_show[i].sequence.size(); ++j ) {
            rval += inp_mngr.get_keyname( inputs_to_show[i].sequence[j], inputs_to_show[i].type );
        }

        // We're generating a list separated by "," and "or"
        if( i + 2 == inputs_to_show.size() ) {
            rval += _( " or " );
        } else if( i + 1 < inputs_to_show.size() ) {
            rval += ", ";
        }
    }
    return rval;
}

std::string input_context::get_desc( const std::string &action_descriptor,
                                     const std::string &text,
                                     const input_context::input_event_filter &evt_filter ) const
{
    if( action_descriptor == "ANY_INPUT" ) {
        // \u00A0 is the non-breaking space
        //~ keybinding description for anykey
        return string_format( pgettext( "keybinding", "[any]\u00A0%s" ), text );
    }

    const auto &events = inp_mngr.get_input_for_action( action_descriptor, category );

    bool na = true;
    for( const auto &evt : events ) {
        if( is_event_type_enabled( evt.type ) && evt_filter( evt ) ) {
            na = false;
            if( ( evt.type == input_event_t::keyboard_char || evt.type == input_event_t::keyboard_code ) &&
                evt.modifiers.empty() && evt.sequence.size() == 1 ) {
                const int ch = evt.get_first_input();
                if( ch > ' ' && ch <= '~' ) {
                    const std::string key = utf32_to_utf8( ch );
                    const auto pos = ci_find_substr( text, key );
                    if( pos >= 0 ) {
                        return text.substr( 0, pos ) + "(" + key + ")" + text.substr( pos + key.size() );
                    }
                }
            }
        }
    }

    if( na ) {
        //~ keybinding description for unbound or disabled keys
        return string_format( pgettext( "keybinding", "[n/a]\u00A0%s" ), text );
    } else {
        //~ keybinding description for bound keys
        return string_format( pgettext( "keybinding", "[%s]\u00A0%s" ),
                              get_desc( action_descriptor, 1, evt_filter ), text );
    }
}

std::string input_context::describe_key_and_name( const std::string &action_descriptor,
        const input_context::input_event_filter &evt_filter ) const
{
    return get_desc( action_descriptor, get_action_name( action_descriptor ), evt_filter );
}

const std::string &input_context::handle_input()
{
    return handle_input( timeout );
}

const std::string &input_context::handle_input( const int timeout )
{
    const auto old_timeout = inp_mngr.get_timeout();
    if( timeout >= 0 ) {
        inp_mngr.set_timeout( timeout );
    }
    next_action.type = input_event_t::error;
    const std::string *result = &CATA_ERROR;
    while( true ) {
        next_action = inp_mngr.get_input_event( preferred_keyboard_mode );
        if( next_action.type == input_event_t::timeout ) {
            result = &TIMEOUT;
            break;
        }

        const std::string &action = input_to_action( next_action );

        // Special help action
        if( action == "HELP_KEYBINDINGS" ) {
            inp_mngr.reset_timeout();
            display_menu();
            inp_mngr.set_timeout( timeout );
            result = &HELP_KEYBINDINGS;
            break;
        }

        if( next_action.type == input_event_t::mouse ) {
            if( !handling_coordinate_input && action == CATA_ERROR ) {
                continue; // Ignore this mouse input.
            }

            coordinate_input_received = true;
            coordinate = next_action.mouse_pos;
        } else {
            coordinate_input_received = false;
        }

        if( action != CATA_ERROR ) {
            result = &action;
            break;
        }

        // If we registered to receive any input, return ANY_INPUT
        // to signify that an unregistered key was pressed.
        if( registered_any_input ) {
            result = &ANY_INPUT;
            break;
        }

        // If it's an invalid key, just keep looping until the user
        // enters something proper.
    }
    inp_mngr.set_timeout( old_timeout );
    return *result;
}

void input_context::register_directions()
{
    register_cardinal();
    register_action( "LEFTUP" );
    register_action( "LEFTDOWN" );
    register_action( "RIGHTUP" );
    register_action( "RIGHTDOWN" );
}

void input_context::register_updown()
{
    register_action( "UP" );
    register_action( "DOWN" );
}

void input_context::register_leftright()
{
    register_action( "LEFT" );
    register_action( "RIGHT" );
}

void input_context::register_cardinal()
{
    register_updown();
    register_leftright();
}

// dx and dy are -1, 0, or +1. Rotate the indicated direction 1/8 turn clockwise.
void rotate_direction_cw( int &dx, int &dy )
{
    // convert to
    // 0 1 2
    // 3 4 5
    // 6 7 8
    int dir_num = ( dy + 1 ) * 3 + dx + 1;
    // rotate to
    // 1 2 5
    // 0 4 8
    // 3 6 7
    static const std::array<int, 9> rotate_direction_vec = {{ 1, 2, 5, 0, 4, 8, 3, 6, 7 }};
    dir_num = rotate_direction_vec[dir_num];
    // convert back to -1,0,+1
    dx = dir_num % 3 - 1;
    dy = dir_num / 3 - 1;
}

cata::optional<tripoint> input_context::get_direction( const std::string &action ) const
{
    static const auto noop = static_cast<tripoint( * )( tripoint )>( []( tripoint p ) {
        return p;
    } );
    static const auto rotate = static_cast<tripoint( * )( tripoint )>( []( tripoint p ) {
        rotate_direction_cw( p.x, p.y );
        return p;
    } );
    const auto transform = iso_mode && tile_iso && use_tiles ? rotate : noop;

    if( action == "UP" ) {
        return transform( tripoint_north );
    } else if( action == "DOWN" ) {
        return transform( tripoint_south );
    } else if( action == "LEFT" ) {
        return transform( tripoint_west );
    } else if( action == "RIGHT" ) {
        return transform( tripoint_east );
    } else if( action == "LEFTUP" ) {
        return transform( tripoint_north_west );
    } else if( action == "RIGHTUP" ) {
        return transform( tripoint_north_east );
    } else if( action == "LEFTDOWN" ) {
        return transform( tripoint_south_west );
    } else if( action == "RIGHTDOWN" ) {
        return transform( tripoint_south_east );
    } else {
        return cata::nullopt;
    }
}

// Custom set of hotkeys that explicitly don't include the hardcoded
// alternative hotkeys, which mustn't be included so that the hardcoded
// hotkeys do not show up beside entries within the window.
const std::string display_help_hotkeys =
    "abcdefghijkpqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:;'\",/<>?!@#$%^&*()_[]\\{}|`~";

action_id input_context::display_menu( const bool permit_execute_action )
{
    action_id action_to_execute = ACTION_NULL;

    // Shamelessly stolen from help.cpp
    input_context ctxt( "HELP_KEYBINDINGS", keyboard_mode::keychar );
    ctxt.register_action( "UP", to_translation( "Scroll up" ) );
    ctxt.register_action( "DOWN", to_translation( "Scroll down" ) );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "REMOVE" );
    ctxt.register_action( "ADD_LOCAL" );
    ctxt.register_action( "ADD_GLOBAL" );
    ctxt.register_action( "EXECUTE" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "ANY_INPUT" );

    if( category != "HELP_KEYBINDINGS" ) {
        // avoiding inception!
        ctxt.register_action( "HELP_KEYBINDINGS" );
    }

    std::string hotkeys = ctxt.get_available_single_char_hotkeys( display_help_hotkeys );

    ui_adaptor ui;
    int width = 0;
    int height = 0;
    catacurses::window w_help;
    size_t display_height = 0;
    size_t legwidth = 0;
    string_input_popup spopup;
    const auto recalc_size = [&]( ui_adaptor & ui ) {
        int maxwidth = std::max( FULL_SCREEN_WIDTH, TERMX );
        width = min( 80, maxwidth );
        int maxheight = std::max( FULL_SCREEN_HEIGHT, TERMY );
        height = min( maxheight, static_cast<int>( hotkeys.size() ) + LEGEND_HEIGHT + BORDER_SPACE );

        w_help = catacurses::newwin( height - 2, width - 2,
                                     point( maxwidth / 2 - width / 2, maxheight / 2 - height / 2 ) );
        // height of the area usable for display of keybindings, excludes headers & borders
        display_height = height - LEGEND_HEIGHT - BORDER_SPACE; // -2 for the border
        // width of the legend
        legwidth = width - 4 - BORDER_SPACE;
        spopup.window( w_help, point( 4, 8 ), legwidth )
        .max_length( legwidth )
        .context( ctxt );
        ui.position_from_window( w_help );
    };
    recalc_size( ui );
    ui.on_screen_resize( recalc_size );

    // has the user changed something?
    bool changed = false;
    // keybindings before the user changed anything.
    input_manager::t_action_contexts old_action_contexts( inp_mngr.action_contexts );
    // current status: adding/removing/executing/showing keybindings
    enum { s_remove, s_add, s_add_global, s_execute, s_show } status = s_show;
    // copy of registered_actions, but without the ANY_INPUT and COORDINATE, which should not be shown
    std::vector<std::string> org_registered_actions( registered_actions );
    org_registered_actions.erase( std::remove_if( org_registered_actions.begin(),
                                  org_registered_actions.end(),
    []( const std::string & a ) {
        return a == ANY_INPUT || a == COORDINATE;
    } ), org_registered_actions.end() );

    // colors of the keybindings
    static const nc_color global_key = c_light_gray;
    static const nc_color local_key = c_light_green;
    static const nc_color unbound_key = c_light_red;
    // (vertical) scroll offset
    size_t scroll_offset = 0;
    // keybindings help
    std::string legend;
    legend += colorize( _( "Unbound keys" ), unbound_key ) + "\n";
    legend += colorize( _( "Keybinding active only on this screen" ), local_key ) + "\n";
    legend += colorize( _( "Keybinding active globally" ), global_key ) + "\n";
    legend += _( "Press - to remove keybinding\nPress + to add local keybinding\nPress = to add global keybinding\n" );
    if( permit_execute_action ) {
        legend += _( "Press . to execute action\n" );
    }

    std::vector<std::string> filtered_registered_actions = org_registered_actions;
    std::string filter_phrase;
    std::string action;
    int raw_input_char = 0;

    const auto redraw = [&]( const ui_adaptor & ) {
        werase( w_help );
        draw_border( w_help, BORDER_COLOR, _( "Keybindings" ), c_light_red );
        draw_scrollbar( w_help, scroll_offset, display_height,
                        filtered_registered_actions.size(), point( 0, 10 ), c_white, true );
        fold_and_print( w_help, point( 2, 1 ), legwidth, c_white, legend );

        for( size_t i = 0; i + scroll_offset < filtered_registered_actions.size() &&
             i < display_height; i++ ) {
            const std::string &action_id = filtered_registered_actions[i + scroll_offset];

            bool overwrite_default;
            const action_attributes &attributes = inp_mngr.get_action_attributes( action_id, category,
                                                  &overwrite_default );

            char invlet;
            if( i < hotkeys.size() ) {
                invlet = hotkeys[i];
            } else {
                invlet = ' ';
            }

            if( status == s_add_global && overwrite_default ) {
                // We're trying to add a global, but this action has a local
                // defined, so gray out the invlet.
                mvwprintz( w_help, point( 2, i + 10 ), c_dark_gray, "%c ", invlet );
            } else if( status == s_add || status == s_add_global ) {
                mvwprintz( w_help, point( 2, i + 10 ), c_light_blue, "%c ", invlet );
            } else if( status == s_remove ) {
                mvwprintz( w_help, point( 2, i + 10 ), c_light_blue, "%c ", invlet );
            } else if( status == s_execute ) {
                mvwprintz( w_help, point( 2, i + 10 ), c_white, "%c ", invlet );
            } else {
                mvwprintz( w_help, point( 2, i + 10 ), c_blue, "  " );
            }
            nc_color col;
            if( attributes.input_events.empty() ) {
                col = unbound_key;
            } else if( overwrite_default ) {
                col = local_key;
            } else {
                col = global_key;
            }
            mvwprintz( w_help, point( 4, i + 10 ), col, "%s:", get_action_name( action_id ) );
            mvwprintz( w_help, point( 52, i + 10 ), col, "%s", get_desc( action_id ) );
        }

        // spopup.query_string() will call wnoutrefresh( w_help )
        spopup.text( filter_phrase );
        spopup.query_string( false, true );
    };
    ui.on_redraw( redraw );

    while( true ) {
        ui_manager::redraw();

        if( status == s_show ) {
            filter_phrase = spopup.query_string( false );
            action = ctxt.input_to_action( ctxt.get_raw_input() );
        } else {
            action = ctxt.handle_input();
        }
        raw_input_char = ctxt.get_raw_input().get_first_input();

        filtered_registered_actions = filter_strings_by_phrase( org_registered_actions, filter_phrase );
        if( scroll_offset > filtered_registered_actions.size() ) {
            scroll_offset = 0;
        }

        if( filtered_registered_actions.empty() && action != "QUIT" ) {
            continue;
        }

        // In addition to the modifiable hotkeys, we also check for hardcoded
        // keys, e.g. '+', '-', '=', '.' in order to prevent the user from
        // entering an unrecoverable state.
        if( action == "ADD_LOCAL" || raw_input_char == '+' ) {
            status = s_add;
        } else if( action == "ADD_GLOBAL" || raw_input_char == '=' ) {
            status = s_add_global;
        } else if( action == "REMOVE" || raw_input_char == '-' ) {
            status = s_remove;
        } else if( ( action == "EXECUTE" || raw_input_char == '.' ) && permit_execute_action ) {
            status = s_execute;
        } else if( action == "ANY_INPUT" ) {
            const size_t hotkey_index = hotkeys.find_first_of( raw_input_char );
            if( hotkey_index == std::string::npos ) {
                continue;
            }
            const size_t action_index = hotkey_index + scroll_offset;
            if( action_index >= filtered_registered_actions.size() ) {
                continue;
            }
            const std::string &action_id = filtered_registered_actions[action_index];

            // Check if this entry is local or global.
            bool is_local = false;
            const action_attributes &actions = inp_mngr.get_action_attributes( action_id, category, &is_local );
            bool is_empty = actions.input_events.empty();
            const std::string name = get_action_name( action_id );

            // We don't want to completely delete a global context entry.
            // Only attempt removal for a local context, or when there's
            // bindings for the default context.
            if( status == s_remove && ( is_local || !is_empty ) ) {
                if( !get_option<bool>( "QUERY_KEYBIND_REMOVAL" ) || query_yn( is_local &&
                        is_empty ? _( "Reset to global bindings for %s?" ) : _( "Clear keys for %s?" ), name ) ) {

                    // If it's global, reset the global actions.
                    std::string category_to_access = category;
                    if( !is_local ) {
                        category_to_access = default_context_id;
                    }

                    inp_mngr.remove_input_for_action( action_id, category_to_access );
                    changed = true;
                }
            } else if( status == s_add_global && is_local ) {
                // Disallow adding global actions to an action that already has a local defined.
                popup( _( "There are already local keybindings defined for this action, please remove them first." ) );
            } else if( status == s_add || status == s_add_global ) {
                const input_event new_event = query_popup()
                                              .preferred_keyboard_mode( preferred_keyboard_mode )
                                              .message( _( "New key for %s" ), name )
                                              .allow_anykey( true )
                                              .query()
                                              .evt;

                if( action_uses_input( action_id, new_event ) ) {
                    popup_getkey( _( "This key is already used for %s." ), name );
                    status = s_show;
                    continue;
                }

                const std::string conflicts = get_conflicts( new_event );
                const bool has_conflicts = !conflicts.empty();
                bool resolve_conflicts = false;

                if( has_conflicts ) {
                    resolve_conflicts = query_yn(
                                            _( "This key conflicts with %s. Remove this key from the conflicting command(s), and continue?" ),
                                            conflicts.c_str() );
                }

                if( !has_conflicts || resolve_conflicts ) {
                    if( resolve_conflicts ) {
                        clear_conflicting_keybindings( new_event );
                    }

                    // We might be adding a local or global action.
                    std::string category_to_access = category;
                    if( status == s_add_global ) {
                        category_to_access = default_context_id;
                    }

                    inp_mngr.add_input_for_action( action_id, category_to_access, new_event );
                    changed = true;
                }
            } else if( status == s_execute && permit_execute_action ) {
                action_to_execute = look_up_action( action_id );
                break;
            }
            status = s_show;
        } else if( action == "DOWN" ) {
            if( filtered_registered_actions.size() > display_height &&
                scroll_offset < filtered_registered_actions.size() - display_height ) {
                scroll_offset++;
            }
        } else if( action == "UP" ) {
            if( scroll_offset > 0 ) {
                scroll_offset--;
            }
        } else if( action == "PAGE_DOWN" ) {
            if( scroll_offset + display_height < filtered_registered_actions.size() ) {
                scroll_offset += std::min( display_height, filtered_registered_actions.size() -
                                           display_height - scroll_offset );
            } else if( filtered_registered_actions.size() > display_height ) {
                scroll_offset = 0;
            }
        } else if( action == "PAGE_UP" ) {
            if( scroll_offset >= display_height ) {
                scroll_offset -= display_height;
            } else if( scroll_offset > 0 ) {
                scroll_offset = 0;
            } else if( filtered_registered_actions.size() > display_height ) {
                scroll_offset = filtered_registered_actions.size() - display_height;
            }
        } else if( action == "QUIT" ) {
            if( status != s_show ) {
                status = s_show;
            } else {
                break;
            }
        } else if( action == "HELP_KEYBINDINGS" ) {
            // update available hotkeys in case they've changed
            hotkeys = ctxt.get_available_single_char_hotkeys( display_help_hotkeys );
        }
    }

    if( changed && query_yn( _( "Save changes?" ) ) ) {
        try {
            inp_mngr.save();
            get_help().load();
        } catch( std::exception &err ) {
            popup( _( "saving keybindings failed: %s" ), err.what() );
        }
    } else if( changed ) {
        inp_mngr.action_contexts.swap( old_action_contexts );
    }

    return action_to_execute;
}

input_event input_context::get_raw_input()
{
    return next_action;
}

int input_manager::get_previously_pressed_key() const
{
    return previously_pressed_key;
}

void input_manager::wait_for_any_key()
{
#if defined(__ANDROID__)
    input_context ctxt( "WAIT_FOR_ANY_KEY", keyboard_mode::keycode );
#endif
    while( true ) {
        const input_event evt = inp_mngr.get_input_event( keyboard_mode::keycode );
        switch( evt.type ) {
            case input_event_t::keyboard_char:
                if( !evt.sequence.empty() ) {
                    return;
                }
                break;
            case input_event_t::keyboard_code:
                return;
            // errors are accepted as well to avoid an infinite loop
            case input_event_t::error:
                return;
            default:
                break;
        }
    }
}

#if !(defined(TILES) || defined(_WIN32))
// Also specify that we don't have a gamepad plugged in.
bool gamepad_available()
{
    return false;
}

cata::optional<tripoint> input_context::get_coordinates( const catacurses::window &capture_win )
{
    if( !coordinate_input_received ) {
        return cata::nullopt;
    }
    const point view_size( getmaxx( capture_win ), getmaxy( capture_win ) );
    const point win_min( getbegx( capture_win ),
                         getbegy( capture_win ) );
    const half_open_rectangle win_bounds( win_min, win_min + view_size );
    if( !win_bounds.contains( coordinate ) ) {
        return cata::nullopt;
    }

    point view_offset;
    if( capture_win == g->w_terrain ) {
        view_offset = g->ter_view_p.xy();
    }

    const point p = view_offset - ( view_size / 2 - coordinate );
    return tripoint( p, g->get_levz() );
}
#endif

std::pair<point, bool> input_context::get_coordinates_text( const catacurses::window
        &capture_win ) const
{
#if !defined( TILES )
    ( void ) capture_win;
    return std::make_pair( point(), false );
#else
    if( !coordinate_input_received ) {
        return std::make_pair( point(), false );
    }

    const window_dimensions dim = get_window_dimensions( capture_win );

    const int &fw = dim.scaled_font_size.x;
    const int &fh = dim.scaled_font_size.y;
    const point &win_min = dim.window_pos_pixel;
    const point &win_size = dim.window_size_pixel;
    const point win_max = win_min + win_size;

    const half_open_rectangle win_bounds( win_min, win_max );

    const point screen_pos = coordinate - win_min;
    const point selected( divide_round_down( screen_pos.x, fw ),
                          divide_round_down( screen_pos.y, fh ) );

    if( !win_bounds.contains( coordinate ) ) {
        return std::make_pair( selected, false );
    }

    return std::make_pair( selected, true );
#endif
}

std::string input_context::get_action_name( const std::string &action_id ) const
{
    // 1) Check action name overrides specific to this input_context
    const auto action_name_override =
        action_name_overrides.find( action_id );
    if( action_name_override != action_name_overrides.end() ) {
        return action_name_override->second.translated();
    }

    // 2) Check if the hotkey has a name
    const action_attributes &attributes = inp_mngr.get_action_attributes( action_id, category );
    if( !attributes.name.empty() ) {
        return attributes.name.translated();
    }

    // 3) If the hotkey has no name, the user has created a local hotkey in
    // this context that is masking the global hotkey. Fallback to the global
    // hotkey's name.
    const action_attributes &default_attributes = inp_mngr.get_action_attributes( action_id,
            default_context_id );
    if( !default_attributes.name.empty() ) {
        return default_attributes.name.translated();
    }

    // 4) Unable to find suitable name. Keybindings configuration likely borked
    return action_id;
}

// (Press X (or Y)|Try) to Z
std::string input_context::press_x( const std::string &action_id ) const
{
    return press_x( action_id, _( "Press " ), "", _( "Try" ) );
}

std::string input_context::press_x( const std::string &action_id, const std::string &key_bound,
                                    const std::string &key_unbound ) const
{
    return press_x( action_id, key_bound, "", key_unbound );
}

// TODO: merge this with input_context::get_desc
std::string input_context::press_x( const std::string &action_id, const std::string &key_bound_pre,
                                    const std::string &key_bound_suf, const std::string &key_unbound ) const
{
    if( action_id == "ANY_INPUT" ) {
        return _( "any key" );
    }
    if( action_id == "COORDINATE" ) {
        return _( "mouse movement" );
    }
    input_manager::t_input_event_list events = inp_mngr.get_input_for_action( action_id, category );
    events.erase( std::remove_if( events.begin(), events.end(), [this]( const input_event & evt ) {
        return !is_event_type_enabled( evt.type );
    } ), events.end() );
    if( events.empty() ) {
        return key_unbound;
    }
    std::string keyed = key_bound_pre;
    for( size_t j = 0; j < events.size(); j++ ) {
        // test in fixed order to generate consistent description
        for( const auto &v : keymod_desc ) {
            if( events[j].modifiers.count( v.first ) ) {
                keyed += v.second.translated();
            }
        }
        for( size_t k = 0; k < events[j].sequence.size(); ++k ) {
            keyed += inp_mngr.get_keyname( events[j].sequence[k], events[j].type );
        }
        if( j + 1 < events.size() ) {
            keyed += _( " or " );
        }
    }
    keyed += key_bound_suf;
    return keyed;
}

void input_context::set_iso( bool mode )
{
    iso_mode = mode;
}

std::vector<std::string> input_context::filter_strings_by_phrase(
    const std::vector<std::string> &strings, const std::string &phrase ) const
{
    std::vector<std::string> filtered_strings;

    for( auto &str : strings ) {
        if( lcmatch( remove_color_tags( get_action_name( str ) ), phrase ) ) {
            filtered_strings.push_back( str );
        }
    }

    return filtered_strings;
}

void input_context::set_edittext( const std::string &s )
{
    edittext = s;
}

std::string input_context::get_edittext()
{
    return edittext;
}

void input_context::set_timeout( int val )
{
    timeout = val;
}

void input_context::reset_timeout()
{
    timeout = -1;
}

bool input_context::is_event_type_enabled( const input_event_t type ) const
{
    switch( type ) {
        case input_event_t::error:
            return false;
        case input_event_t::timeout:
            return true;
        case input_event_t::keyboard_char:
            return preferred_keyboard_mode == keyboard_mode::keychar || !is_keycode_mode_supported();
        case input_event_t::keyboard_code:
            return preferred_keyboard_mode == keyboard_mode::keycode && is_keycode_mode_supported();
        case input_event_t::gamepad:
            return gamepad_available();
        case input_event_t::mouse:
            return true;
    }
    return true;
}
