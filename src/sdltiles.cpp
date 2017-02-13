#if (defined TILES)
#include "catacurse.h"
#include "options.h"
#include "output.h"
#include "input.h"
#include "color.h"
#include "catacharset.h"
#include "cursesdef.h"
#include "debug.h"
#include "player.h"
#include "translations.h"
#include <cstring>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <memory>
#include <stdexcept>
#include <limits>
#include "cata_tiles.h"
#include "get_version.h"
#include "init.h"
#include "path_info.h"
#include "filesystem.h"
#include "map.h"
#include "game.h"
#include "lightmap.h"
#include "rng.h"
#include <algorithm>

//TODO replace these includes with filesystem.h
#ifdef _MSC_VER
#   include "wdirent.h"
#   include <direct.h>
#else
#   include <dirent.h>
#endif

#if (defined _WIN32 || defined WINDOWS)
#   include "platform_win.h"
#   include <shlwapi.h>
#   ifndef strcasecmp
#       define strcasecmp StrCmpI
#   endif
#endif

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>

#ifdef SDL_SOUND
#   include <SDL_mixer.h>
#   include "sounds.h"
#endif

#ifdef __ANDROID__
#include "worldfactory.h"
#include "action.h"
#include "vehicle.h"
#include "inventory.h"
#include <jni.h>
#endif

#define dbg(x) DebugLog((DebugLevel)(x),D_SDL) << __FILE__ << ":" << __LINE__ << ": "

//***********************************
//Globals                           *
//***********************************

std::unique_ptr<cata_tiles> tilecontext;
static unsigned long lastupdate = 0;
static unsigned long interval = 25;
static bool needupdate = false;
extern bool tile_iso;
extern WINDOW *w_hit_animation;

#ifdef SDL_SOUND
/** The music we're currently playing. */
Mix_Music *current_music = NULL;
std::string current_playlist = "";
size_t current_playlist_at = 0;
size_t absolute_playlist_at = 0;
std::vector<std::size_t> playlist_indexes;

struct sound_effect {
    int volume;

    struct deleter {
        // Operator overloaded to leverage deletion API.
        void operator()( Mix_Chunk* const c ) const {
            Mix_FreeChunk( c );
        };
    };
    std::unique_ptr<Mix_Chunk, deleter> chunk;
};

using id_and_variant = std::pair<std::string, std::string>;
std::map<id_and_variant, std::vector<sound_effect>> sound_effects_p;

struct music_playlist {
    // list of filenames relative to the soundpack location
    struct entry {
        std::string file;
        int volume;
    };
    std::vector<entry> entries;
    bool shuffle;

    music_playlist() : shuffle(false) {
    }
};

std::map<std::string, music_playlist> playlists;

std::string current_soundpack_path = "";
#endif

/**
 * A class that draws a single character on screen.
 */
class Font {
public:
    Font(int w, int h) : 
#ifdef __ANDROID__
    opacity(1.0f),
#endif
    fontwidth(w), fontheight(h) { }
    virtual ~Font() { }
    /**
     * Draw character t at (x,y) on the screen,
     * using (curses) color.
     */
    virtual void OutputChar(std::string ch, int x, int y, unsigned char color) = 0;
    virtual void draw_ascii_lines(unsigned char line_id, int drawx, int drawy, int FG) const;
    bool draw_window(WINDOW *win);
    bool draw_window(WINDOW *win, int offsetx, int offsety);

    static std::unique_ptr<Font> load_font(const std::string &typeface, int fontsize, int fontwidth, int fontheight);
public:
#ifdef __ANDROID__
    float opacity; // 0-1
#endif
    // the width of the font, background is always this size
    int fontwidth;
    // the height of the font, background is always this size
    int fontheight;
};

/**
 * Uses a ttf font. Its glyphs are cached.
 */
class CachedTTFFont : public Font {
public:
    CachedTTFFont(int w, int h);
    virtual ~CachedTTFFont();

    void clear();
    void load_font(std::string typeface, int fontsize);
    virtual void OutputChar(std::string ch, int x, int y, unsigned char color);
protected:
    SDL_Texture *create_glyph(const std::string &ch, int color);

    TTF_Font* font;
    // Maps (character code, color) to SDL_Texture*

    struct key_t {
        std::string   codepoints;
        unsigned char color;

        // Operator overload required to use in std::map.
        bool operator<(key_t const &rhs) const noexcept {
            return (color == rhs.color) ? codepoints < rhs.codepoints : color < rhs.color;
        }
    };

    struct cached_t {
        SDL_Texture* texture;
        int          width;
    };

    std::map<key_t, cached_t> glyph_cache_map;
};

/**
 * A font created from a bitmap. Each character is taken from a
 * specific area of the source bitmap.
 */
class BitmapFont : public Font {
public:
    BitmapFont(int w, int h);
    virtual ~BitmapFont();

    void clear();
    void load_font(const std::string &path);
    virtual void OutputChar(std::string ch, int x, int y, unsigned char color);
    void OutputChar(long t, int x, int y, unsigned char color);
    virtual void draw_ascii_lines(unsigned char line_id, int drawx, int drawy, int FG) const;
protected:
    SDL_Texture *ascii[16];
    int tilewidth;
};

static std::unique_ptr<Font> font;
static std::unique_ptr<Font> map_font;
static std::unique_ptr<Font> overmap_font;

std::array<std::string, 16> main_color_names{ { "BLACK","RED","GREEN","BROWN","BLUE","MAGENTA",
"CYAN","GRAY","DGRAY","LRED","LGREEN","YELLOW","LBLUE","LMAGENTA","LCYAN","WHITE" } };
static std::array<SDL_Color, 256> windowsPalette;
static SDL_Window *window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_PixelFormat *format;
static SDL_Texture *display_buffer;
#ifdef __ANDROID__
static SDL_Texture *touch_joystick;
#endif
int WindowWidth;        //Width of the actual window, not the curses window
int WindowHeight;       //Height of the actual window, not the curses window
// input from various input sources. Each input source sets the type and
// the actual input value (key pressed, mouse button clicked, ...)
// This value is finally returned by input_manager::get_input_event.
input_event last_input;

int inputdelay;         //How long getch will wait for a character to be typed
Uint32 delaydpad = std::numeric_limits<Uint32>::max();     // Used for entering diagonal directions with d-pad.
Uint32 dpad_delay = 100;   // Delay in milli-seconds between registering a d-pad event and processing it.
bool dpad_continuous = false;  // Whether we're currently moving continously with the dpad.
int lastdpad = ERR;      // Keeps track of the last dpad press.
int queued_dpad = ERR;   // Queued dpad press, for individual button presses.
//WINDOW *_windows;  //Probably need to change this to dynamic at some point
//int WindowCount;        //The number of curses windows currently in use
int fontwidth;          //the width of the font, background is always this size
int fontheight;         //the height of the font, background is always this size
static int TERMINAL_WIDTH;
static int TERMINAL_HEIGHT;
std::map< std::string,std::vector<int> > consolecolors;

static SDL_Joystick *joystick; // Only one joystick for now.

static bool fontblending = false;

// Cache of bitmap fonts family.
// Used only while fontlist.txt is created.
static std::set<std::string> bitmap_fonts;

static std::vector<curseline> oversized_framebuffer;
static std::vector<curseline> terminal_framebuffer;
static WINDOW *winBuffer; //tracking last drawn window to fix the framebuffer
static int fontScaleBuffer; //tracking zoom levels to fix framebuffer w/tiles
extern WINDOW *w_hit_animation; //this window overlays w_terrain which can be oversized

//***********************************
//Tile-version specific functions   *
//***********************************

void init_interface()
{
    return; // dummy function, we have nothing to do here
}
//***********************************
//Non-curses, Window functions      *
//***********************************

void ClearScreen()
{
    SDL_RenderClear(renderer);
}

bool InitSDL()
{
    int init_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    int ret;

    ret = SDL_Init( init_flags );
    if( ret != 0 ) {
        dbg( D_ERROR ) << "SDL_Init failed with " << ret << ", error: " << SDL_GetError();
        return false;
    }
    ret = TTF_Init();
    if( ret != 0 ) {
        dbg( D_ERROR ) << "TTF_Init failed with " << ret << ", error: " << TTF_GetError();
        return false;
    }
    ret = IMG_Init( IMG_INIT_PNG );
    if( (ret & IMG_INIT_PNG) != IMG_INIT_PNG ) {
        dbg( D_ERROR ) << "IMG_Init failed to initialize PNG support, tiles won't work, error: " << IMG_GetError();
        // cata_tiles won't be able to load the tiles, but the normal SDL
        // code will display fine.
    }

    ret = SDL_InitSubSystem( SDL_INIT_JOYSTICK );
    if( ret != 0 ) {
        dbg( D_WARNING ) << "Initializing joystick subsystem failed with " << ret << ", error: " <<
        SDL_GetError() << "\nIf you don't have a joystick plugged in, this is probably fine.";
    }

    //SDL2 has no functionality for INPUT_DELAY, we would have to query it manually, which is expensive
    //SDL2 instead uses the OS's Input Delay.

    atexit(SDL_Quit);

    return true;
}

bool SetupRenderTarget()
{
    if( SDL_SetRenderDrawBlendMode( renderer, SDL_BLENDMODE_NONE ) != 0 ) {
        dbg( D_ERROR ) << "SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) failed: " << SDL_GetError();
        // Ignored for now, rendering could still work
    }
    display_buffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, WindowWidth, WindowHeight);
    if( display_buffer == nullptr ) {
        dbg( D_ERROR ) << "Failed to create window buffer: " << SDL_GetError();
        return false;
    }
    if( SDL_SetRenderTarget( renderer, display_buffer ) != 0 ) {
        dbg( D_ERROR ) << "Failed to select render target: " << SDL_GetError();
        return false;
    }

    return true;
}

//Registers, creates, and shows the Window!!
bool WinCreate()
{
    std::string version = string_format("Cataclysm: Dark Days Ahead - %s", getVersionString());

    // Common flags used for fulscreen and for windowed
    int window_flags = 0;
    WindowWidth = TERMINAL_WIDTH * fontwidth;
    WindowHeight = TERMINAL_HEIGHT * fontheight;

    if( get_option<std::string>( "SCALING_MODE" ) != "none" ) {
        window_flags |= SDL_WINDOW_RESIZABLE;
        SDL_SetHint( SDL_HINT_RENDER_SCALE_QUALITY, get_option<std::string>( "SCALING_MODE" ).c_str() );
    }

#ifndef __ANDROID__
    if (get_option<std::string>( "FULLSCREEN" ) == "fullscreen") {
#endif
        window_flags |= SDL_WINDOW_FULLSCREEN;
#ifndef __ANDROID__
    } else if (get_option<std::string>( "FULLSCREEN" ) == "windowedbl") {
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    }
#endif
    
    int display = get_option<int>( "DISPLAY" );
    if ( display < 0 || display >= SDL_GetNumVideoDisplays() ) {
        display = 0;
    }

#ifdef __ANDROID__
    // Bugfix for red screen on Samsung S3/Mali
	// https://forums.libsdl.org/viewtopic.php?t=11445
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5); 
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6); 
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5); 
#endif

    window = SDL_CreateWindow(version.c_str(),
            SDL_WINDOWPOS_CENTERED_DISPLAY( display ),
            SDL_WINDOWPOS_CENTERED_DISPLAY( display ),
            WindowWidth,
            WindowHeight,
            window_flags
        );

    if (window == NULL) {
        dbg(D_ERROR) << "SDL_CreateWindow failed: " << SDL_GetError();
        return false;
    }
#ifndef __ANDROID__
	// On Android SDL seems janky in windowed mode so we're fullscreen all the time.
	// Fullscreen mode is now modified so it obeys terminal width/height, rather than
	// overwriting it with this calculation.
    if (window_flags & SDL_WINDOW_FULLSCREEN || window_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
        SDL_GetWindowSize(window, &WindowWidth, &WindowHeight);
        // Ignore previous values, use the whole window, but nothing more.
        TERMINAL_WIDTH = WindowWidth / fontwidth;
        TERMINAL_HEIGHT = WindowHeight / fontheight;
    }
#endif
    // Initialize framebuffer caches
    terminal_framebuffer.resize(TERMINAL_HEIGHT);
    for (int i = 0; i < TERMINAL_HEIGHT; i++) {
        terminal_framebuffer[i].chars.assign(TERMINAL_WIDTH, cursecell(""));
    }

    oversized_framebuffer.resize(TERMINAL_HEIGHT);
    for (int i = 0; i < TERMINAL_HEIGHT; i++) {
        oversized_framebuffer[i].chars.assign(TERMINAL_WIDTH, cursecell(""));
    }

    const Uint32 wformat = SDL_GetWindowPixelFormat(window);
    format = SDL_AllocFormat(wformat);
    if(format == 0) {
        dbg(D_ERROR) << "SDL_AllocFormat(" << wformat << ") failed: " << SDL_GetError();
        return false;
    }

    bool software_renderer = get_option<bool>( "SOFTWARE_RENDERING" );
    if( !software_renderer ) {
        dbg( D_INFO ) << "Attempting to initialize accelerated SDL renderer.";

        renderer = SDL_CreateRenderer( window, -1, SDL_RENDERER_ACCELERATED |
                                       SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE );
        if( renderer == NULL ) {
            dbg( D_ERROR ) << "Failed to initialize accelerated renderer, falling back to software rendering: " << SDL_GetError();
            software_renderer = true;
        } else if( !SetupRenderTarget() ) {
            dbg( D_ERROR ) << "Failed to initialize display buffer under accelerated rendering, falling back to software rendering.";
            software_renderer = true;
            if (display_buffer != NULL) {
                SDL_DestroyTexture(display_buffer);
                display_buffer = NULL;
            }
            if( renderer != NULL ) {
                SDL_DestroyRenderer( renderer );
                renderer = NULL;
            }
        }
    }
    if( software_renderer ) {
        renderer = SDL_CreateRenderer( window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE );
        if( renderer == NULL ) {
            dbg( D_ERROR ) << "Failed to initialize software renderer: " << SDL_GetError();
            return false;
        } else if( !SetupRenderTarget() ) {
            dbg( D_ERROR ) << "Failed to initialize display buffer under software rendering, unable to continue.";
            return false;
        }
    }

#ifdef __ANDROID__
	// TODO: Not too sure why this works to make fullscreen on Android behave. :/
    if ( window_flags & SDL_WINDOW_FULLSCREEN || window_flags & SDL_WINDOW_FULLSCREEN_DESKTOP ) {
        SDL_GetWindowSize( window, &WindowWidth, &WindowHeight );
    }

    // Load virtual joystick texture
    SDL_Surface* touch_joystick_surface = IMG_Load( "android/joystick.png" );
    if ( !touch_joystick_surface ) {
        throw std::runtime_error(IMG_GetError());
    }
    touch_joystick = SDL_CreateTextureFromSurface( renderer, touch_joystick_surface );
    if( !touch_joystick ) {
        dbg( D_ERROR) << "failed to create texture: " << SDL_GetError();
    }
    SDL_FreeSurface( touch_joystick_surface );
#endif

    ClearScreen();

    // Errors here are ignored, worst case: the option does not work as expected,
    // but that won't crash
    if(get_option<std::string>( "HIDE_CURSOR" ) != "show" && SDL_ShowCursor(-1)) {
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_ShowCursor(SDL_ENABLE);
    }

    // Initialize joysticks.
    int numjoy = SDL_NumJoysticks();

    if( get_option<bool>( "ENABLE_JOYSTICK" ) && numjoy >= 1 ) {
        if( numjoy > 1 ) {
            dbg( D_WARNING ) << "You have more than one gamepads/joysticks plugged in, only the first will be used.";
        }
        joystick = SDL_JoystickOpen(0);
        if( joystick == nullptr ) {
            dbg( D_ERROR ) << "Opening the first joystick failed: " << SDL_GetError();
        } else {
            const int ret = SDL_JoystickEventState(SDL_ENABLE);
            if( ret < 0 ) {
                dbg( D_ERROR ) << "SDL_JoystickEventState(SDL_ENABLE) failed: " << SDL_GetError();
            }
        }
    } else {
        joystick = NULL;
    }

    // Set up audio mixer.
#ifdef SDL_SOUND
    int audio_rate = 44100;
    Uint16 audio_format = AUDIO_S16;
    int audio_channels = 2;
    int audio_buffers = 2048;

    if(Mix_OpenAudio(audio_rate, audio_format, audio_channels, audio_buffers)) {
        dbg( D_ERROR ) << "Failed to open audio mixer, sound won't work: " << Mix_GetError();
    }
    Mix_AllocateChannels(128);
    Mix_ReserveChannels(20);

    // For the sound effects system.
    Mix_GroupChannels( 2, 9, 1 );
    Mix_GroupChannels( 0, 1, 2 );
    Mix_GroupChannels( 11, 14, 3 );
    Mix_GroupChannels( 15, 17, 4 );
#endif

    return true;
}

// forward declaration
void load_soundset();
void cleanup_sound();

void WinDestroy()
{
#ifdef __ANDROID__
	if ( touch_joystick ) {
	    SDL_DestroyTexture( touch_joystick );
		touch_joystick = NULL;
	}
#endif

#ifdef SDL_SOUND
    // De-allocate all loaded sound.
    cleanup_sound();
    Mix_CloseAudio();
#endif
    clear_texture_pool();

    if(joystick) {
        SDL_JoystickClose(joystick);
        joystick = 0;
    }
    if(format)
        SDL_FreeFormat(format);
    format = NULL;
    if (display_buffer != NULL) {
        SDL_DestroyTexture(display_buffer);
        display_buffer = NULL;
    }
    if( renderer != NULL ) {
        SDL_DestroyRenderer( renderer );
        renderer = NULL;
    }
    if(window)
        SDL_DestroyWindow(window);
    window = NULL;
}

inline void FillRectDIB(SDL_Rect &rect, unsigned char color) {
    if( SDL_SetRenderDrawColor( renderer, windowsPalette[color].r, windowsPalette[color].g,
                                windowsPalette[color].b, 255 ) != 0 ) {
        dbg(D_ERROR) << "SDL_SetRenderDrawColor failed: " << SDL_GetError();
    }
    if( SDL_RenderFillRect( renderer, &rect ) != 0 ) {
        dbg(D_ERROR) << "SDL_RenderFillRect failed: " << SDL_GetError();
    }
}

//The following 3 methods use mem functions for fast drawing
inline void VertLineDIB(int x, int y, int y2, int thickness, unsigned char color)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = thickness;
    rect.h = y2-y;
    FillRectDIB(rect, color);
}
inline void HorzLineDIB(int x, int y, int x2, int thickness, unsigned char color)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = x2-x;
    rect.h = thickness;
    FillRectDIB(rect, color);
}
inline void FillRectDIB(int x, int y, int width, int height, unsigned char color)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = width;
    rect.h = height;
    FillRectDIB(rect, color);
}


SDL_Texture *CachedTTFFont::create_glyph(const std::string &ch, int color)
{
    SDL_Surface * sglyph = (fontblending ? TTF_RenderUTF8_Blended : TTF_RenderUTF8_Solid)(font, ch.c_str(), windowsPalette[color]);
    if (sglyph == NULL) {
        dbg( D_ERROR ) << "Failed to create glyph for " << ch << ": " << TTF_GetError();
        return NULL;
    }
    /* SDL interprets each pixel as a 32-bit number, so our masks must depend
       on the endianness (byte order) of the machine */
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    static const Uint32 rmask = 0xff000000;
    static const Uint32 gmask = 0x00ff0000;
    static const Uint32 bmask = 0x0000ff00;
    static const Uint32 amask = 0x000000ff;
#else
    static const Uint32 rmask = 0x000000ff;
    static const Uint32 gmask = 0x0000ff00;
    static const Uint32 bmask = 0x00ff0000;
    static const Uint32 amask = 0xff000000;
#endif
    const int wf = utf8_wrapper( ch ).display_width();
    // Note: bits per pixel must be 8 to be synchron with the surface
    // that TTF_RenderGlyph above returns. This is important for SDL_BlitScaled
    SDL_Surface *surface = SDL_CreateRGBSurface(0, fontwidth * wf, fontheight, 32,
                                                rmask, gmask, bmask, amask);
    if (surface == NULL) {
        dbg( D_ERROR ) << "CreateRGBSurface failed: " << SDL_GetError();
        SDL_Texture *glyph = SDL_CreateTextureFromSurface(renderer, sglyph);
        SDL_FreeSurface(sglyph);
        return glyph;
    }
    SDL_Rect src_rect = { 0, 0, sglyph->w, sglyph->h };
    SDL_Rect dst_rect = { 0, 0, fontwidth * wf, fontheight };
    if (src_rect.w < dst_rect.w) {
        dst_rect.x = (dst_rect.w - src_rect.w) / 2;
        dst_rect.w = src_rect.w;
    } else if (src_rect.w > dst_rect.w) {
        src_rect.x = (src_rect.w - dst_rect.w) / 2;
        src_rect.w = dst_rect.w;
    }
    if (src_rect.h < dst_rect.h) {
        dst_rect.y = (dst_rect.h - src_rect.h) / 2;
        dst_rect.h = src_rect.h;
    } else if (src_rect.h > dst_rect.h) {
        src_rect.y = (src_rect.h - dst_rect.h) / 2;
        src_rect.h = dst_rect.h;
    }

    if (SDL_BlitSurface(sglyph, &src_rect, surface, &dst_rect) != 0) {
        dbg( D_ERROR ) << "SDL_BlitSurface failed: " << SDL_GetError();
        SDL_FreeSurface(surface);
    } else {
        SDL_FreeSurface(sglyph);
        sglyph = surface;
    }

    SDL_Texture *glyph = SDL_CreateTextureFromSurface(renderer, sglyph);
    SDL_FreeSurface(sglyph);
    return glyph;
}

void CachedTTFFont::OutputChar(std::string ch, int const x, int const y, unsigned char const color)
{
    key_t    key {std::move(ch), static_cast<unsigned char>(color & 0xf)};
    cached_t value;

    auto const it = glyph_cache_map.lower_bound(key);
    if (it != std::end(glyph_cache_map) && !glyph_cache_map.key_comp()(key, it->first)) {
        value = it->second;
    } else {
        value.texture = create_glyph(key.codepoints, key.color);
        value.width = fontwidth * utf8_wrapper(key.codepoints).display_width();
        glyph_cache_map.insert(it, std::make_pair(std::move(key), value));
    }

    if (!value.texture) {
        // Nothing we can do here )-:
        return;
    }
    SDL_Rect rect {x, y, value.width, fontheight};
#ifdef __ANDROID__
    if (opacity != 1.0f)
        SDL_SetTextureAlphaMod(value.texture, opacity * 255.0f);
#endif
    if (SDL_RenderCopy( renderer, value.texture, nullptr, &rect)) {
        dbg(D_ERROR) << "SDL_RenderCopy failed: " << SDL_GetError();
    }
#ifdef __ANDROID__
    if (opacity != 1.0f)
        SDL_SetTextureAlphaMod(value.texture, 255);
#endif
}

void BitmapFont::OutputChar(std::string ch, int x, int y, unsigned char color)
{
    int len = ch.length();
    const char *s = ch.c_str();
    const long t = UTF8_getch(&s, &len);
    BitmapFont::OutputChar(t, x, y, color);
}

void BitmapFont::OutputChar(long t, int x, int y, unsigned char color)
{
    if( t > 256 ) {
        return;
    }
    SDL_Rect src;
    src.x = (t % tilewidth) * fontwidth;
    src.y = (t / tilewidth) * fontheight;
    src.w = fontwidth;
    src.h = fontheight;
    SDL_Rect rect;
    rect.x = x; rect.y = y; rect.w = fontwidth; rect.h = fontheight;
#ifdef __ANDROID__
    if (opacity != 1.0f)
        SDL_SetTextureAlphaMod(ascii[color], opacity * 255);
#endif
    if( SDL_RenderCopy( renderer, ascii[color], &src, &rect ) != 0 ) {
        dbg(D_ERROR) << "SDL_RenderCopy failed: " << SDL_GetError();
    }
#ifdef __ANDROID__
    if (opacity != 1.0f)
        SDL_SetTextureAlphaMod(ascii[color], 255);
#endif
}

#ifdef __ANDROID__
void draw_quick_shortcuts();
void draw_virtual_joystick();

extern "C" {

static bool visible_display_frame_dirty = false;
static bool has_visible_display_frame = false;
static SDL_Rect visible_display_frame;

JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_onNativeVisibleDisplayFrameChanged(
                                    JNIEnv* env, jclass jcls, jint left, jint top, jint right, jint bottom)
{
    (void)env; // unused
    (void)jcls; // unused
    //LOGD("onNativeVisibleDisplayFrameChanged(): %d %d %d %d", left, top, right, bottom );
    has_visible_display_frame = true;
    visible_display_frame_dirty = true;
    visible_display_frame.x = left;
    visible_display_frame.y = top;
    visible_display_frame.w = right - left;
    visible_display_frame.h = bottom - top;
}

}
#endif

void refresh_display()
{
    needupdate = false;
    lastupdate = SDL_GetTicks();

    // Select default target (the window), copy rendered buffer
    // there, present it, select the buffer as target again.
    if( SDL_SetRenderTarget( renderer, NULL ) != 0 ) {
        dbg(D_ERROR) << "SDL_SetRenderTarget failed: " << SDL_GetError();
    }
    SDL_RenderSetLogicalSize( renderer, WindowWidth, WindowHeight );
#ifdef __ANDROID__
	// If the display buffer aspect ratio is wider than the display, 
	// draw it at the top of the screen so it doesn't get covered up
	// by the virtual keyboard. Otherwise just center it.
    int DisplayBufferWidth = TERMINAL_WIDTH * fontwidth;
    int DisplayBufferHeight = TERMINAL_HEIGHT * fontheight;
    SDL_Rect dstrect;
    float DisplayBufferAspect = DisplayBufferWidth / (float)DisplayBufferHeight;
    float WindowAspect = WindowWidth / (float)WindowHeight;
    if (WindowAspect < DisplayBufferAspect)
    {
        dstrect.x = 0;
        dstrect.y = 0;
        dstrect.w = WindowWidth;
        dstrect.h = WindowWidth / DisplayBufferAspect;
    }
    else
    {
        dstrect.x = 0.5f * (WindowWidth - (WindowHeight * DisplayBufferAspect));
        dstrect.y = 0;
        dstrect.w = WindowHeight * DisplayBufferAspect;
        dstrect.h = WindowHeight;
    }

	// Make sure the destination rectangle fits within the visible area
    if (get_option<bool>("ANDROID_KEYBOARD_SCREEN_SCALE") && has_visible_display_frame) {
        int vdf_right = visible_display_frame.x + visible_display_frame.w;
        int vdf_bottom = visible_display_frame.y + visible_display_frame.h;
        if (vdf_right < dstrect.x + dstrect.w)
            dstrect.w = vdf_right - dstrect.x;
        if (vdf_bottom < dstrect.y + dstrect.h)
            dstrect.h = vdf_bottom - dstrect.y;
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); 
    SDL_RenderClear( renderer );
    if( SDL_RenderCopy( renderer, display_buffer, NULL, &dstrect ) != 0 ) {
#else
    if( SDL_RenderCopy( renderer, display_buffer, NULL, NULL ) != 0 ) {
#endif
        dbg(D_ERROR) << "SDL_RenderCopy failed: " << SDL_GetError();
    }
#ifdef __ANDROID__
    // Draw quick shortcuts on top of the game view
    draw_quick_shortcuts();
    draw_virtual_joystick();
#endif
    SDL_RenderPresent(renderer);
    if( SDL_SetRenderTarget( renderer, display_buffer ) != 0 ) {
        dbg(D_ERROR) << "SDL_SetRenderTarget failed: " << SDL_GetError();
    }
}

// only update if the set interval has elapsed
static void try_sdl_update()
{
    unsigned long now = SDL_GetTicks();
    if (now - lastupdate >= interval) {
        refresh_display();
    } else {
        needupdate = true;
    }
}

//for resetting the render target after updating texture caches in cata_tiles.cpp
void set_displaybuffer_rendertarget()
{
    if( SDL_SetRenderTarget( renderer, display_buffer ) != 0 ) {
        dbg(D_ERROR) << "SDL_SetRenderTarget failed: " << SDL_GetError();
    }
}

// Populate a map with the available video displays and their name
void find_videodisplays() {
    std::map<int, std::string> displays;

    int numdisplays = SDL_GetNumVideoDisplays();
    for( int i = 0 ; i < numdisplays ; i++ ) {
        displays.insert( { i, SDL_GetDisplayName( i ) } );
    }

    int current_display = get_option<int>( "DISPLAY" );
    get_options().add("DISPLAY", "graphics", _("Display"),
                      _("Sets which video display will be used to show the game. Requires restart."),
                      displays, current_display, 0, options_manager::COPT_CURSES_HIDE
                      );
}

// line_id is one of the LINE_*_C constants
// FG is a curses color
void Font::draw_ascii_lines(unsigned char line_id, int drawx, int drawy, int FG) const
{
    switch (line_id) {
        case LINE_OXOX_C://box bottom/top side (horizontal line)
            HorzLineDIB(drawx, drawy + (fontheight / 2), drawx + fontwidth, 1, FG);
            break;
        case LINE_XOXO_C://box left/right side (vertical line)
            VertLineDIB(drawx + (fontwidth / 2), drawy, drawy + fontheight, 2, FG);
            break;
        case LINE_OXXO_C://box top left
            HorzLineDIB(drawx + (fontwidth / 2), drawy + (fontheight / 2), drawx + fontwidth, 1, FG);
            VertLineDIB(drawx + (fontwidth / 2), drawy + (fontheight / 2), drawy + fontheight, 2, FG);
            break;
        case LINE_OOXX_C://box top right
            HorzLineDIB(drawx, drawy + (fontheight / 2), drawx + (fontwidth / 2), 1, FG);
            VertLineDIB(drawx + (fontwidth / 2), drawy + (fontheight / 2), drawy + fontheight, 2, FG);
            break;
        case LINE_XOOX_C://box bottom right
            HorzLineDIB(drawx, drawy + (fontheight / 2), drawx + (fontwidth / 2), 1, FG);
            VertLineDIB(drawx + (fontwidth / 2), drawy, drawy + (fontheight / 2) + 1, 2, FG);
            break;
        case LINE_XXOO_C://box bottom left
            HorzLineDIB(drawx + (fontwidth / 2), drawy + (fontheight / 2), drawx + fontwidth, 1, FG);
            VertLineDIB(drawx + (fontwidth / 2), drawy, drawy + (fontheight / 2) + 1, 2, FG);
            break;
        case LINE_XXOX_C://box bottom north T (left, right, up)
            HorzLineDIB(drawx, drawy + (fontheight / 2), drawx + fontwidth, 1, FG);
            VertLineDIB(drawx + (fontwidth / 2), drawy, drawy + (fontheight / 2), 2, FG);
            break;
        case LINE_XXXO_C://box bottom east T (up, right, down)
            VertLineDIB(drawx + (fontwidth / 2), drawy, drawy + fontheight, 2, FG);
            HorzLineDIB(drawx + (fontwidth / 2), drawy + (fontheight / 2), drawx + fontwidth, 1, FG);
            break;
        case LINE_OXXX_C://box bottom south T (left, right, down)
            HorzLineDIB(drawx, drawy + (fontheight / 2), drawx + fontwidth, 1, FG);
            VertLineDIB(drawx + (fontwidth / 2), drawy + (fontheight / 2), drawy + fontheight, 2, FG);
            break;
        case LINE_XXXX_C://box X (left down up right)
            HorzLineDIB(drawx, drawy + (fontheight / 2), drawx + fontwidth, 1, FG);
            VertLineDIB(drawx + (fontwidth / 2), drawy, drawy + fontheight, 2, FG);
            break;
        case LINE_XOXX_C://box bottom east T (left, down, up)
            VertLineDIB(drawx + (fontwidth / 2), drawy, drawy + fontheight, 2, FG);
            HorzLineDIB(drawx, drawy + (fontheight / 2), drawx + (fontwidth / 2), 1, FG);
            break;
        default:
            break;
    }
}

void invalidate_framebuffer( std::vector<curseline> &framebuffer, int x, int y, int width, int height )
{
    for( int j = 0, fby = y; j < height; j++, fby++ ) {
        std::fill_n( framebuffer[fby].chars.begin() + x, width, cursecell( "" ) );
    }
}

void invalidate_framebuffer( std::vector<curseline> &framebuffer )
{
    for( unsigned int i = 0; i < framebuffer.size(); i++ ) {
        std::fill_n( framebuffer[i].chars.begin(), framebuffer[i].chars.size(), cursecell( "" ) );
    }
}

void invalidate_all_framebuffers()
{
    invalidate_framebuffer( terminal_framebuffer );
    invalidate_framebuffer( oversized_framebuffer );
}

void reinitialize_framebuffer()
{
    //Re-initialize the framebuffer with new values.
    const int new_height = std::max( TERMY, std::max( OVERMAP_WINDOW_HEIGHT, TERRAIN_WINDOW_HEIGHT ) );
    const int new_width = std::max( TERMX, std::max( OVERMAP_WINDOW_WIDTH, TERRAIN_WINDOW_WIDTH ) );
    oversized_framebuffer.resize( new_height );
    for( int i = 0; i < new_height; i++ ) {
        oversized_framebuffer[i].chars.assign( new_width, cursecell( "" ) );
    }
    terminal_framebuffer.resize( new_height );
    for( int i = 0; i < new_height; i++ ) {
        terminal_framebuffer[i].chars.assign( new_width, cursecell( "" ) );
    }
}

void invalidate_framebuffer_proportion( WINDOW* win )
{
    const int oversized_width = std::max( TERMX, std::max( OVERMAP_WINDOW_WIDTH, TERRAIN_WINDOW_WIDTH ) );
    const int oversized_height = std::max( TERMY, std::max( OVERMAP_WINDOW_HEIGHT, TERRAIN_WINDOW_HEIGHT ) );

    // check if the framebuffers/windows have been prepared yet
    if ( oversized_height == 0 || oversized_width == 0 ) {
        return;
    }
    if ( !g || win == nullptr ) {
        return;
    }
    if ( win == g->w_overmap || win == g->w_terrain || win == w_hit_animation ) {
        return;
    }

    // track the dimensions for conversion
    const int termpixel_x = win->x * font->fontwidth;
    const int termpixel_y = win->y * font->fontheight;
    const int termpixel_x2 = termpixel_x + win->width * font->fontwidth - 1;
    const int termpixel_y2 = termpixel_y + win->height * font->fontheight - 1;

    if ( map_font != nullptr && map_font->fontwidth != 0 && map_font->fontheight != 0 ) {
        const int mapfont_x = termpixel_x / map_font->fontwidth;
        const int mapfont_y = termpixel_y / map_font->fontheight;
        const int mapfont_x2 = std::min( termpixel_x2 / map_font->fontwidth, oversized_width - 1 );
        const int mapfont_y2 = std::min( termpixel_y2 / map_font->fontheight, oversized_height - 1 );
        const int mapfont_width = mapfont_x2 - mapfont_x + 1;
        const int mapfont_height = mapfont_y2 - mapfont_y + 1;
        invalidate_framebuffer( oversized_framebuffer, mapfont_x, mapfont_y, mapfont_width, mapfont_height );
    }

    if ( overmap_font != nullptr && overmap_font->fontwidth != 0 && overmap_font->fontheight != 0 ) {
        const int overmapfont_x = termpixel_x / overmap_font->fontwidth;
        const int overmapfont_y = termpixel_y / overmap_font->fontheight;
        const int overmapfont_x2 = std::min( termpixel_x2 / overmap_font->fontwidth, oversized_width - 1 );
        const int overmapfont_y2 = std::min( termpixel_y2 / overmap_font->fontheight, oversized_height - 1 );
        const int overmapfont_width = overmapfont_x2 - overmapfont_x + 1;
        const int overmapfont_height = overmapfont_y2 - overmapfont_y + 1;
        invalidate_framebuffer( oversized_framebuffer, overmapfont_x, overmapfont_y, overmapfont_width, overmapfont_height );
    }
}

// clear the framebuffer when werase is called on certain windows that don't use the main terminal font
void handle_additional_window_clear( WINDOW* win )
{
    if ( !g ) {
        return;
    }
    if( win == g->w_terrain || win == g->w_overmap ){
        invalidate_framebuffer( oversized_framebuffer );
    }
}

void clear_window_area(WINDOW* win)
{
    FillRectDIB(win->x * fontwidth, win->y * fontheight,
                win->width * fontwidth, win->height * fontheight, COLOR_BLACK);
}

void curses_drawwindow(WINDOW *win)
{
    bool update = false;
    if (g && win == g->w_terrain && use_tiles) {
        // game::w_terrain can be drawn by the tilecontext.
        // skip the normal drawing code for it.
        tilecontext->draw(
            win->x * fontwidth,
            win->y * fontheight,
            tripoint( g->ter_view_x, g->ter_view_y, g->ter_view_z ),
            TERRAIN_WINDOW_TERM_WIDTH * font->fontwidth,
            TERRAIN_WINDOW_TERM_HEIGHT * font->fontheight);

        invalidate_framebuffer(terminal_framebuffer, win->x, win->y, TERRAIN_WINDOW_TERM_WIDTH, TERRAIN_WINDOW_TERM_HEIGHT);

        update = true;
    } else if (g && win == g->w_terrain && map_font ) {
        // When the terrain updates, predraw a black space around its edge
        // to keep various former interface elements from showing through the gaps
        // TODO: Maybe track down screen changes and use g->w_blackspace to draw this instead

        //calculate width differences between map_font and font
        int partial_width = std::max(TERRAIN_WINDOW_TERM_WIDTH * fontwidth - TERRAIN_WINDOW_WIDTH * map_font->fontwidth, 0);
        int partial_height = std::max(TERRAIN_WINDOW_TERM_HEIGHT * fontheight - TERRAIN_WINDOW_HEIGHT * map_font->fontheight, 0);
        //Gap between terrain and lower window edge
        if( partial_height > 0 ) {
            FillRectDIB( win->x * map_font->fontwidth,
                         ( win->y + TERRAIN_WINDOW_HEIGHT ) * map_font->fontheight,
                         TERRAIN_WINDOW_WIDTH * map_font->fontwidth + partial_width, partial_height, COLOR_BLACK );
        }
        //Gap between terrain and sidebar
        if( partial_width > 0 ) {
            FillRectDIB( ( win->x + TERRAIN_WINDOW_WIDTH ) * map_font->fontwidth, win->y * map_font->fontheight,
                         partial_width, TERRAIN_WINDOW_HEIGHT * map_font->fontheight + partial_height, COLOR_BLACK );
        }
        // Special font for the terrain window
        update = map_font->draw_window(win);
    } else if (g && win == g->w_overmap && overmap_font ) {
        // Special font for the terrain window
        update = overmap_font->draw_window(win);
    } else if (win == w_hit_animation && map_font ) {
        // The animation window overlays the terrain window,
        // it uses the same font, but it's only 1 square in size.
        // The offset must not use the global font, but the map font
        int offsetx = win->x * map_font->fontwidth;
        int offsety = win->y * map_font->fontheight;
        update = map_font->draw_window(win, offsetx, offsety);
    } else if (g && win == g->w_blackspace) {
        // fill-in black space window skips draw code
        // so as not to confuse framebuffer any more than necessary
        int offsetx = win->x * font->fontwidth;
        int offsety = win->y * font->fontheight;
        int wwidth = win->width * font->fontwidth;
        int wheight = win->height * font->fontheight;
        FillRectDIB(offsetx, offsety, wwidth, wheight, COLOR_BLACK);
        update = true;
    } else if (g && win == g->w_pixel_minimap && g->pixel_minimap_option) {
        // Make sure the entire minimap window is black before drawing.
        clear_window_area(win);
        tilecontext->draw_minimap(
            win->x * fontwidth, win->y * fontheight,
            tripoint( g->u.pos().x, g->u.pos().y, g->ter_view_z ),
            win->width * font->fontwidth, win->height * font->fontheight);
        update = true;
    } else {
        // Either not using tiles (tilecontext) or not the w_terrain window.
        update = font->draw_window(win);
    }
    if(update) {
        needupdate = true;
    }
}

bool Font::draw_window(WINDOW *win)
{
    // Use global font sizes here to make this independent of the
    // font used for this window.
    return draw_window(win, win->x * ::fontwidth, win->y * ::fontheight);
}

bool Font::draw_window( WINDOW *win, int offsetx, int offsety )
{
    //Keeping track of the last drawn window
    if( winBuffer == NULL ) {
            winBuffer = win;
    }
    if( !fontScaleBuffer ) {
            fontScaleBuffer = tilecontext->get_tile_width();
    }
    const int fontScale = tilecontext->get_tile_width();
    //This creates a problem when map_font is different from the regular font
    //Specifically when showing the overmap
    //And in some instances of screen change, i.e. inventory.
    bool oldWinCompatible = false;

    // clear the oversized buffer proportionally
    invalidate_framebuffer_proportion( win );

    // use the oversize buffer when dealing with windows that can have a different font than the main text font
    bool use_oversized_framebuffer = g && ( win == g->w_terrain || win == g->w_overmap ||
                                            ( win != nullptr && win == w_hit_animation ) );

    /*
    Let's try to keep track of different windows.
    A number of windows are coexisting on the screen, so don't have to interfere.

    g->w_terrain, g->w_minimap, g->w_HP, g->w_status, g->w_status2, g->w_messages,
     g->w_location, and g->w_minimap, can be buffered if either of them was
     the previous window.

    g->w_overmap and g->w_omlegend are likewise.

    Everything else works on strict equality because there aren't yet IDs for some of them.
    */
    if( g && ( win == g->w_terrain || win == g->w_minimap || win == g->w_HP || win == g->w_status ||
         win == g->w_status2 || win == g->w_messages || win == g->w_location ) ) {
        if ( winBuffer == g->w_terrain || winBuffer == g->w_minimap ||
             winBuffer == g->w_HP || winBuffer == g->w_status || winBuffer == g->w_status2 ||
             winBuffer == g->w_messages || winBuffer == g->w_location ) {
            oldWinCompatible = true;
        }
    }else if( g && ( win == g->w_overmap || win == g->w_omlegend ) ) {
        if ( winBuffer == g->w_overmap || winBuffer == g->w_omlegend ) {
            oldWinCompatible = true;
        }
    }else {
        if( win == winBuffer ) {
            oldWinCompatible = true;
        }
    }

    bool update = false;
    for( int j = 0; j < win->height; j++ ) {
        if( !win->line[j].touched ) {
            continue;
        }
        update = true;
        win->line[j].touched = false;
        for( int i = 0; i < win->width; i++ ) {
            const cursecell &cell = win->line[j].chars[i];

            const int drawx = offsetx + i * fontwidth;
            const int drawy = offsety + j * fontheight;
            if( drawx + fontwidth > WindowWidth || drawy + fontheight > WindowHeight ) {
                // Outside of the display area, would not render anyway
                continue;
            }

            // Avoid redrawing an unchanged tile by checking the framebuffer cache
            // TODO: handle caching when drawing normal windows over graphical tiles
            const int fbx = win->x + i;
            const int fby = win->y + j;

            std::vector<curseline> &framebuffer = use_oversized_framebuffer ? oversized_framebuffer :
                                             terminal_framebuffer;

#ifdef __ANDROID__
			// BUGFIX: Prevents an occasional crash when viewing player info. This seems like it might be a cross-platform issue in the experimental build
            if (fby >= (int)framebuffer.size() || fbx >= (int)framebuffer[fby].chars.size())
                continue;
#endif
            cursecell &oldcell = framebuffer[fby].chars[fbx];

            if (oldWinCompatible && cell == oldcell && fontScale == fontScaleBuffer) {
                continue;
            }
            oldcell = cell;

            if( cell.ch.empty() ) {
                continue; // second cell of a multi-cell character
            }
            const char *utf8str = cell.ch.c_str();
            int len = cell.ch.length();
            const int codepoint = UTF8_getch( &utf8str, &len );
            const int FG = cell.FG;
            const int BG = cell.BG;
            if( codepoint != UNKNOWN_UNICODE ) {
                const int cw = utf8_width( cell.ch );
                if( cw < 1 ) {
                    // utf8_width() may return a negative width
                    continue;
                }
                FillRectDIB( drawx, drawy, fontwidth * cw, fontheight, BG );
                OutputChar( cell.ch, drawx, drawy, FG );
            } else {
                FillRectDIB( drawx, drawy, fontwidth, fontheight, BG );
                draw_ascii_lines( static_cast<unsigned char>( cell.ch[0] ), drawx, drawy, FG );
            }

        }
    }
    win->draw = false; //We drew the window, mark it as so
    //Keeping track of last drawn window and tilemode zoom level
    winBuffer = win;
    fontScaleBuffer = tilecontext->get_tile_width();

    return update;
}

static long alt_buffer = 0;
static bool alt_down = false;

static void begin_alt_code()
{
    alt_buffer = 0;
    alt_down = true;
}

static bool add_alt_code( char c )
{
    if( alt_down && c >= '0' && c <= '9' ) {
        alt_buffer = alt_buffer * 10 + ( c - '0' );
        return true;
    }
    return false;
}

static long end_alt_code()
{
    alt_down = false;
    return alt_buffer;
}

int HandleDPad()
{
    // Check if we have a gamepad d-pad event.
    if(SDL_JoystickGetHat(joystick, 0) != SDL_HAT_CENTERED) {
        // When someone tries to press a diagonal, they likely will
        // press a single direction first. Wait a few milliseconds to
        // give them time to press both of the buttons for the diagonal.
        int button = SDL_JoystickGetHat(joystick, 0);
        int lc = ERR;
        if(button == SDL_HAT_LEFT) {
            lc = JOY_LEFT;
        } else if(button == SDL_HAT_DOWN) {
            lc = JOY_DOWN;
        } else if(button == SDL_HAT_RIGHT) {
            lc = JOY_RIGHT;
        } else if(button == SDL_HAT_UP) {
            lc = JOY_UP;
        } else if(button == SDL_HAT_LEFTUP) {
            lc = JOY_LEFTUP;
        } else if(button == SDL_HAT_LEFTDOWN) {
            lc = JOY_LEFTDOWN;
        } else if(button == SDL_HAT_RIGHTUP) {
            lc = JOY_RIGHTUP;
        } else if(button == SDL_HAT_RIGHTDOWN) {
            lc = JOY_RIGHTDOWN;
        }

        if( delaydpad == std::numeric_limits<Uint32>::max() ) {
            delaydpad = SDL_GetTicks() + dpad_delay;
            queued_dpad = lc;
        }

        // Okay it seems we're ready to process.
        if( SDL_GetTicks() > delaydpad ) {

            if(lc != ERR) {
                if(dpad_continuous && (lc & lastdpad) == 0) {
                    // Continuous movement should only work in the same or similar directions.
                    dpad_continuous = false;
                    lastdpad = lc;
                    return 0;
                }

                last_input = input_event(lc, CATA_INPUT_GAMEPAD);
                lastdpad = lc;
                queued_dpad = ERR;

                if(dpad_continuous == false) {
                    delaydpad = SDL_GetTicks() + 200;
                    dpad_continuous = true;
                } else {
                    delaydpad = SDL_GetTicks() + 60;
                }
                return 1;
            }
        }
    } else {
        dpad_continuous = false;
        delaydpad = std::numeric_limits<Uint32>::max();

        // If we didn't hold it down for a while, just
        // fire the last registered press.
        if(queued_dpad != ERR) {
            last_input = input_event(queued_dpad, CATA_INPUT_GAMEPAD);
            queued_dpad = ERR;
            return 1;
        }
    }

    return 0;
}

/**
 * Translate SDL key codes to key identifiers used by ncurses, this
 * allows the input_manager to only consider those.
 * @return 0 if the input can not be translated (unknown key?),
 * -1 when a ALT+number sequence has been started,
 * or somthing that a call to ncurses getch would return.
 */
long sdl_keysym_to_curses(SDL_Keysym keysym)
{
    switch (keysym.sym) {
        // This is special: allow entering a unicode character with ALT+number
        case SDLK_RALT:
        case SDLK_LALT:
            begin_alt_code();
            return -1;
        // The following are simple translations:
        case SDLK_KP_ENTER:
        case SDLK_RETURN:
        case SDLK_RETURN2:
            return '\n';
        case SDLK_BACKSPACE:
        case SDLK_KP_BACKSPACE:
            return KEY_BACKSPACE;
        case SDLK_DELETE:
            return KEY_DC;
        case SDLK_ESCAPE:
            return KEY_ESCAPE;
        case SDLK_TAB:
            if (keysym.mod & KMOD_SHIFT) {
                return KEY_BTAB;
            }
            return '\t';
        case SDLK_LEFT:
            return KEY_LEFT;
        case SDLK_RIGHT:
            return KEY_RIGHT;
        case SDLK_UP:
            return KEY_UP;
        case SDLK_DOWN:
            return KEY_DOWN;
        case SDLK_PAGEUP:
            return KEY_PPAGE;
        case SDLK_PAGEDOWN:
            return KEY_NPAGE;
        case SDLK_HOME:
            return KEY_HOME;
        case SDLK_END:
            return KEY_END;
        case SDLK_F1: return KEY_F(1);
        case SDLK_F2: return KEY_F(2);
        case SDLK_F3: return KEY_F(3);
        case SDLK_F4: return KEY_F(4);
        case SDLK_F5: return KEY_F(5);
        case SDLK_F6: return KEY_F(6);
        case SDLK_F7: return KEY_F(7);
        case SDLK_F8: return KEY_F(8);
        case SDLK_F9: return KEY_F(9);
        case SDLK_F10: return KEY_F(10);
        case SDLK_F11: return KEY_F(11);
        case SDLK_F12: return KEY_F(12);
        case SDLK_F13: return KEY_F(13);
        case SDLK_F14: return KEY_F(14);
        case SDLK_F15: return KEY_F(15);
        // Every other key is ignored as there is no curses constant for it.
        // TODO: add more if you find more.
        default:
            return 0;
    }
}

#ifdef __ANDROID__
static float finger_down_x = -1.0f; // in pixels
static float finger_down_y = -1.0f; // in pixels
static float finger_curr_x = -1.0f; // in pixels
static float finger_curr_y = -1.0f; // in pixels
static float second_finger_down_x = -1.0f; // in pixels
static float second_finger_down_y = -1.0f; // in pixels
static float second_finger_curr_x = -1.0f; // in pixels
static float second_finger_curr_y = -1.0f; // in pixels
static unsigned long finger_down_time = 0; // when did the first finger start touching the screen? 0 if not touching, otherwise the time in milliseconds.
static unsigned long finger_repeat_time = 0; // the last time we repeated input for a finger hold, 0 if not touching, otherwise the time in milliseconds.
static unsigned long last_tap_time = 0; // the last time a single tap was detected. used for double-tap detection.
static unsigned long ac_back_down_time = 0; // when did the hardware back button start being pressed? 0 if not touching, otherwise the time in milliseconds.
static bool is_two_finger_touch = false; // has a second finger touched the screen while the first was touching?
static bool is_quick_shortcut_touch = false; // did this touch start on a quick shortcut?
static bool quick_shortcuts_enabled = true;
static bool quick_shortcuts_toggle_handled = false;
unsigned long finger_repeat_delay = 500; // the current finger repeat delay - will be somewhere between the min/max values depending on user input

// Quick shortcuts container: maps the touch input context category (std::string) to a std::list of input_events.
typedef std::list<input_event> quick_shortcuts_t;
std::map<std::string, quick_shortcuts_t> quick_shortcuts_map;

// A copy of the last known input_context from the input manager. It's important this is a copy, as there are times
// the input manager has an empty input_context (eg. when player is moving over slow objects) and we don't want our
// quick shortcuts to disappear momentarily.
input_context touch_input_context;

std::string get_quick_shortcut_name(const std::string& category) {
    if( category == "DEFAULTMODE" && g->check_zone( "NO_AUTO_PICKUP", g->u.pos() ) && get_option<bool>("ANDROID_SHORTCUT_ZONE"))
        return "DEFAULTMODE____SHORTCUTS";
    return category;
}

// given the active quick shortcuts, returns the dimensions of each quick shortcut button.
void get_quick_shortcut_dimensions(quick_shortcuts_t& qsl, float& border, float& width, float& height) {
    border = std::floor(get_option<int>( "ANDROID_SHORTCUT_BORDER" ));
    width = get_option<int>( "ANDROID_SHORTCUT_WIDTH_MAX" );
    float min_width = std::min(get_option<int>( "ANDROID_SHORTCUT_WIDTH_MIN" ), get_option<int>( "ANDROID_SHORTCUT_WIDTH_MAX" ));
    if (width * qsl.size() > WindowWidth) {
        width *= WindowWidth / (width * qsl.size());
        if (width < min_width)
            width = min_width;
    }
    width = std::floor(width);
    height = std::floor(get_option<int>( "ANDROID_SHORTCUT_HEIGHT" ));
}

// Returns the quick shortcut (if any) under the finger's current position, or finger down position if down == true
input_event* get_quick_shortcut_under_finger(bool down = false) {

    if (!quick_shortcuts_enabled)
        return NULL;

    quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(touch_input_context.get_category())];

    float border, width, height;
    get_quick_shortcut_dimensions(qsl, border, width, height);

    float finger_y = down ? finger_down_y : finger_curr_y;
    if (finger_y < WindowHeight - height)
        return NULL;

    int i = 0;
    bool shortcut_right = get_option<std::string>( "ANDROID_SHORTCUT_POSITION" ) == "right";
    float finger_x = down ? finger_down_x : finger_curr_x;
    for (std::list<input_event>::iterator it = qsl.begin(); it != qsl.end(); ++it) {
        i++;
        if (shortcut_right) {
            if (finger_x > WindowWidth - (i * width))
                return &(*it);
        }
        else {
        if (finger_x < i * width)
            return &(*it);
        }
    }

    return NULL;
}

// when pre-populating a quick shortcut list with defaults, ignore these actions (since they're all handleable by native touch operations)
bool ignore_action_for_quick_shortcuts(const std::string& action) {
   return (action == "UP"
        || action == "DOWN"
        || action == "LEFT"
        || action == "RIGHT"
        || action == "LEFTUP"
        || action == "LEFTDOWN"
        || action == "RIGHTUP"
        || action == "RIGHTDOWN"
        || action == "QUIT"
        || action == "CONFIRM"
        || action == "MOVE_SINGLE_ITEM" // maps to ENTER
        || action == "MOVE_ARMOR" // maps to ENTER
        || action == "ANY_INPUT"
        || action == "DELETE_TEMPLATE" // strictly we shouldn't have this one, but I don't like seeing the "d" on the main menu by default. :)
        );
}

// Adds a quick shortcut to a quick_shortcut list, setting shortcut_last_used_action_counter accordingly.
void add_quick_shortcut(quick_shortcuts_t& qsl, input_event& event, bool back, bool reset_shortcut_last_used_action_counter) {
    if (reset_shortcut_last_used_action_counter)
        event.shortcut_last_used_action_counter = g->get_user_action_counter(); // only used for DEFAULTMODE
    if (back)
        qsl.push_back(event);
    else
        qsl.push_front(event);
}

// Given a quick shortcut list and a specific key, move that key to the front or back of the list.
void reorder_quick_shortcut(quick_shortcuts_t& qsl, long key, bool back) {
        for(const auto& event : qsl) {
            if (event.get_first_input() == key) {
                input_event event_copy = event;
                qsl.remove(event);
                add_quick_shortcut(qsl, event_copy, back, false);
                break;
            }
        }
}

void reorder_quick_shortcuts(quick_shortcuts_t& qsl) {
        // Do some manual reordering to make transitions between input contexts more consistent
        // Desired order of keys: < > BACKTAB TAB PPAGE NPAGE . . . . ?
        bool shortcut_right = get_option<std::string>( "ANDROID_SHORTCUT_POSITION" ) == "right";
        if (shortcut_right) {
            reorder_quick_shortcut(qsl, KEY_PPAGE, false); // paging control
            reorder_quick_shortcut(qsl, KEY_NPAGE, false);
            reorder_quick_shortcut(qsl, KEY_BTAB, false); // secondary tabs after that
            reorder_quick_shortcut(qsl, '\t', false);
            reorder_quick_shortcut(qsl, '<', false); // tabs next
            reorder_quick_shortcut(qsl, '>', false);
            reorder_quick_shortcut(qsl, '?', false); // help at the start
        }
        else {
            reorder_quick_shortcut(qsl, KEY_NPAGE, false);
            reorder_quick_shortcut(qsl, KEY_PPAGE, false); // paging control
            reorder_quick_shortcut(qsl, '\t', false);
            reorder_quick_shortcut(qsl, KEY_BTAB, false); // secondary tabs after that
            reorder_quick_shortcut(qsl, '>', false);
            reorder_quick_shortcut(qsl, '<', false); // tabs next
            reorder_quick_shortcut(qsl, '?', false); // help at the start
        }
}

long choose_best_key_for_action(const std::string& action, const std::string& category) {
    const std::vector<input_event>& events = inp_mngr.get_input_for_action( action, category );
    long best_key = -1;
    for( const auto &events_event : events ) {
        if( events_event.type == CATA_INPUT_KEYBOARD && events_event.sequence.size() == 1 ) {
            bool is_ascii_char = isprint( events_event.sequence.front() ) && events_event.sequence.front() < 0xFF;
            bool is_best_ascii_char = best_key >= 0 && isprint( best_key ) && best_key < 0xFF;
            if ( best_key < 0 || (is_ascii_char && !is_best_ascii_char) ) {
                //LOGD("\tSelecting new best_key: old best_key: %ld isprint:%d new best_key: %ld isprint:%d", best_key, isprint(best_key), events_event.sequence.front(), isprint(events_event.sequence.front()) );
                best_key = events_event.sequence.front();
            }
        }
    }
    return best_key;
}

bool add_key_to_quick_shortcuts(long key, const std::string& category, bool back) {
    if (key > 0) {
        quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(category)];
        input_event event = input_event(key, CATA_INPUT_KEYBOARD);
        quick_shortcuts_t::iterator it = std::find(qsl.begin(), qsl.end(), event);
        if (it != qsl.end()) { // already exists
            (*it).shortcut_last_used_action_counter = g->get_user_action_counter(); // make sure we refresh shortcut usage
        }
        else {
            add_quick_shortcut(qsl, event, back, true); // doesn't exist, add it to the shortcuts and refresh shortcut usage
            return true;
        }
    }
    return false;
}
bool add_best_key_for_action_to_quick_shortcuts(std::string action_str, const std::string& category, bool back) {
    long best_key = choose_best_key_for_action(action_str, category);
    return add_key_to_quick_shortcuts(best_key, category, back);
}

bool add_best_key_for_action_to_quick_shortcuts(action_id action, const std::string& category, bool back) {
    return add_best_key_for_action_to_quick_shortcuts(action_ident(action), category, back);
}

void remove_action_from_quick_shortcuts(std::string action_str, const std::string& category) {
    quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(category)];
    const std::vector<input_event>& events = inp_mngr.get_input_for_action( action_str, category );
    for (const auto& event : events)
        qsl.remove(event);
}

void remove_action_from_quick_shortcuts(action_id action, const std::string& category) {
    remove_action_from_quick_shortcuts(action_ident(action), category);
}

// Returns true if an expired action was removed
bool remove_expired_actions_from_quick_shortcuts(const std::string& category) {
    int remove_turns = get_option<int>("ANDROID_SHORTCUT_REMOVE_TURNS");
    if (remove_turns <= 0)
        return false;

    // This should only ever be used on "DEFAULTMODE" category for gameplay shortcuts
    if (category != "DEFAULTMODE")
        return false;

	bool ret = false;
    quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(category)];
    quick_shortcuts_t::iterator it = qsl.begin();
    while (it != qsl.end()) {
        if (g->get_user_action_counter() - (*it).shortcut_last_used_action_counter > remove_turns) {
            it = qsl.erase(it);
			ret = true;
		}
        else {
            ++it;
		}
    }
	return ret;
}

void remove_stale_inventory_quick_shortcuts() {
    if (get_option<bool>("ANDROID_INVENTORY_AUTOADD")) {
        quick_shortcuts_t& qsl = quick_shortcuts_map["INVENTORY"];
        quick_shortcuts_t::iterator it = qsl.begin();
        bool in_inventory;
        long key;
        bool valid;
        while (it != qsl.end()) {
            key = (*it).get_first_input();
            valid = inv_chars.valid(key);
            in_inventory = false;
            if (valid) {
                in_inventory = g->u.inv.invlet_to_position(key) != INT_MIN;
                if (!in_inventory) {
                    // We couldn't find this item in the inventory, let's check worn items
                    for (const auto& item : g->u.worn) {
                        if (item.invlet == key) {
                            in_inventory = true;
                            break;
                        }
                    }
                }
                if (!in_inventory) {
                    // We couldn't find it in worn items either, check weapon held
                    if (g->u.weapon.invlet == key)
                        in_inventory = true;
                }
            }
            if (valid && !in_inventory) {
                it = qsl.erase(it);
            }
            else {
                ++it;
            }
        }
    }    
}

void draw_quick_shortcuts() {

    if (!quick_shortcuts_enabled || 
        SDL_IsTextInputActive() ||
        (get_option<bool>("ANDROID_HIDE_HOLDS") && !is_quick_shortcut_touch && finger_down_time > 0 && SDL_GetTicks() - finger_down_time >= (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY"))) // player is swipe + holding in a direction
        return;

    bool shortcut_right = get_option<std::string>( "ANDROID_SHORTCUT_POSITION" ) == "right";
    std::string& category = touch_input_context.get_category();
    bool is_default_mode = category == "DEFAULTMODE";
    quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(category)];
    if (qsl.size() == 0 || touch_input_context.get_registered_manual_keys().size() > 0) {
        if (category == "DEFAULTMODE") {
            const std::string default_gameplay_shortcuts = get_option<std::string>("ANDROID_SHORTCUT_DEFAULTS");
            for (const auto& c : default_gameplay_shortcuts)
                add_key_to_quick_shortcuts(c, category, true);
        }
        else {
            // This is an empty quick-shortcuts list, let's pre-populate it as best we can from the input context
            
            // For manual key lists, force-clear them each time since there's no point allowing custom bindings anyway
            if (touch_input_context.get_registered_manual_keys().size() > 0)
                qsl.clear();

            // First process registered actions
            std::vector<std::string>& registered_actions = touch_input_context.get_registered_actions();
            for (std::vector<std::string>::iterator it = registered_actions.begin(); it != registered_actions.end(); ++it) {
                std::string& action = *it;
                //LOGD("prepopulating action id %s...", action.c_str());
                if (ignore_action_for_quick_shortcuts(action))
                    continue;

                add_best_key_for_action_to_quick_shortcuts(action, category, !shortcut_right);
            }            

            // Then process manual keys
            std::vector<input_context::manual_key>& registered_manual_keys = touch_input_context.get_registered_manual_keys();
            for (const auto& manual_key : registered_manual_keys) {
                //LOGD("prepopulating key %ld %s...", manual_key.key, manual_key.text.c_str());
                input_event event(manual_key.key, CATA_INPUT_KEYBOARD);
                add_quick_shortcut(qsl, event, !shortcut_right, true);
            }
        }
    }

    // Only reorder quick shortcuts for non-gameplay lists that are likely to have navigational menu stuff
    if (!is_default_mode)
        reorder_quick_shortcuts(qsl);

    float border, width, height;
    get_quick_shortcut_dimensions(qsl, border, width, height);
    input_event* hovered_quick_shortcut = get_quick_shortcut_under_finger();
    SDL_Rect rect;
    bool hovered, show_hint;
    int i = 0;
    for (std::list<input_event>::iterator it = qsl.begin(); it != qsl.end(); ++it) {
        input_event& event = *it;
        std::string text = event.text;
        long key = event.get_first_input();
        float default_text_scale = std::floor(0.75f * (height / font->fontheight)); // default for single character strings
        float text_scale = default_text_scale;
        if (text.empty() || text == " ") {
            text = inp_mngr.get_keyname(key, event.type);
            text_scale = std::min(text_scale, 0.75f * (width / (font->fontwidth * text.length())));
        }
        hovered = is_quick_shortcut_touch && hovered_quick_shortcut == &event;
        show_hint = hovered && SDL_GetTicks() - finger_down_time > (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY");
        std::string hint_text;
        if (show_hint) {
            if (touch_input_context.get_category() == "INVENTORY" && inv_chars.valid(key)) {
                // Special case for inventory items - show the inventory item name as help text
                hint_text = g->u.inv.find_item(g->u.inv.invlet_to_position(key)).display_name();
                if (hint_text == "none") {
                    // We couldn't find this item in the inventory, let's check worn items
                    for (const auto& item : g->u.worn) {
                        if (item.invlet == key) {
                            hint_text = item.display_name();
                            break;
                        }
                    }
                }
                if (hint_text == "none") {
                    // We couldn't find it in worn items either, must be weapon held
                    if (g->u.weapon.invlet == key)
                        hint_text = g->u.weapon.display_name();
                }
            }
            else {
                // All other screens - try and show the action name, either from registered actions or manually registered keys
                hint_text = touch_input_context.get_action_name(touch_input_context.input_to_action(event));
                if (hint_text == "ERROR") {
                    hint_text = touch_input_context.get_action_name_for_manual_key(key);
                }
            }
            if (hint_text == "ERROR" || hint_text == "none" || hint_text.empty())
                show_hint = false;
        }
        if (shortcut_right)
            rect = { WindowWidth - (int)((i+1) * width + border), (int)(WindowHeight - height), (int)(width - border*2), (int)(height) };
        else
            rect = { (int)(i * width + border), (int)(WindowHeight - height), (int)(width - border*2), (int)(height) };
        if (hovered)
            SDL_SetRenderDrawColor( renderer, 0, 0, 0, 255 );
        else
            SDL_SetRenderDrawColor( renderer, 0, 0, 0, get_option<int>("ANDROID_SHORTCUT_OPACITY_BG")*0.01f*255.0f );
        SDL_SetRenderDrawBlendMode( renderer, SDL_BLENDMODE_BLEND );
        SDL_RenderFillRect( renderer, &rect );
        if (hovered) {
            // draw a second button hovering above the first one
            if (shortcut_right)
                rect = { WindowWidth - (int)((i+1) * width + border), (int)(WindowHeight - height * 2.2f), (int)(width - border*2), (int)(height) };
            else
                rect = { (int)(i * width + border), (int)(WindowHeight - height * 2.2f), (int)(width - border*2), (int)(height) };
            SDL_SetRenderDrawColor( renderer, 0, 0, 196, 255 );
            SDL_RenderFillRect( renderer, &rect );

            if (show_hint) {
                // draw a backdrop for the hint text
                rect = { 0, (int)((WindowHeight - height)*0.5f), (int)WindowWidth, (int)height };
                SDL_SetRenderDrawColor( renderer, 0, 0, 0, get_option<int>("ANDROID_SHORTCUT_OPACITY_BG")*0.01f*255.0f );
                SDL_RenderFillRect( renderer, &rect );                
            }
        }
        SDL_SetRenderDrawBlendMode( renderer, SDL_BLENDMODE_NONE );
        SDL_RenderSetScale( renderer, text_scale, text_scale);
        int text_x, text_y;
        if (shortcut_right)
            text_x = (WindowWidth - (i + 0.5f) * width - (font->fontwidth * text.length()) * text_scale * 0.5f) / text_scale;
        else
            text_x = ((i + 0.5f) * width - (font->fontwidth * text.length()) * text_scale * 0.5f) / text_scale;
        text_y = (WindowHeight - (height + font->fontheight * text_scale) * 0.5f) / text_scale;
        font->opacity = get_option<int>("ANDROID_SHORTCUT_OPACITY_SHADOW")*0.01f;
        font->OutputChar( text, text_x+1, text_y+1, 0 );
        font->opacity = get_option<int>("ANDROID_SHORTCUT_OPACITY_FG")*0.01f;
        font->OutputChar( text, text_x, text_y, get_option<int>("ANDROID_SHORTCUT_COLOR") );
        if (hovered) {
            // draw a second button hovering above the first one
            font->OutputChar( text, text_x, text_y - (height*1.2f / text_scale), get_option<int>("ANDROID_SHORTCUT_COLOR") );
            if (show_hint) {
                // draw hint text
                text_scale = default_text_scale;
                hint_text = text + " " + hint_text;
                hint_text = remove_color_tags(hint_text);
                const float safe_margin = 0.9f;
                int hint_length = utf8_width(hint_text);
                if (WindowWidth * safe_margin < font->fontwidth * text_scale * hint_length)
                    text_scale *= (WindowWidth * safe_margin) / (font->fontwidth * text_scale * hint_length); // scale to fit comfortably
                SDL_RenderSetScale( renderer, text_scale, text_scale);
                font->opacity = get_option<int>("ANDROID_SHORTCUT_OPACITY_SHADOW")*0.01f;
                text_x = (WindowWidth - ((font->fontwidth  * hint_length) * text_scale)) * 0.5f / text_scale;
                text_y = (WindowHeight - font->fontheight * text_scale) * 0.5f / text_scale;
                font->OutputChar( hint_text, text_x+1, text_y+1, 0 );
                font->opacity = get_option<int>("ANDROID_SHORTCUT_OPACITY_FG")*0.01f;
                font->OutputChar( hint_text, text_x, text_y, get_option<int>("ANDROID_SHORTCUT_COLOR") );
            }
        }
        font->opacity = 1.0f;
        SDL_RenderSetScale( renderer, 1.0f, 1.0f);
        i++;
        if ((i+1) * width > WindowWidth)
            break;
    }
}


void draw_virtual_joystick() {

	// Bail out if we don't need to draw the joystick
    if (!get_option<bool>("ANDROID_SHOW_VIRTUAL_JOYSTICK") || 
        finger_down_time <= 0 || 
        SDL_GetTicks() - finger_down_time <= (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY") || 
        is_quick_shortcut_touch || 
        is_two_finger_touch)
        return;

    SDL_SetTextureAlphaMod( touch_joystick, get_option<int>("ANDROID_VIRTUAL_JOYSTICK_OPACITY")*0.01f*255.0f );

    float longest_window_edge = std::max(WindowWidth, WindowHeight);

    SDL_Rect dstrect;

    // Draw deadzone range
    dstrect.w = dstrect.h = ( get_option<float>("ANDROID_DEADZONE_RANGE") ) * longest_window_edge * 2;
    dstrect.x = finger_down_x - dstrect.w/2;
    dstrect.y = finger_down_y - dstrect.h/2;
    SDL_RenderCopy( renderer, touch_joystick, NULL, &dstrect );

    // Draw repeat delay range
    dstrect.w = dstrect.h = ( get_option<float>("ANDROID_DEADZONE_RANGE") + get_option<float>("ANDROID_REPEAT_DELAY_RANGE") ) * longest_window_edge * 2;
    dstrect.x = finger_down_x - dstrect.w/2;
    dstrect.y = finger_down_y - dstrect.h/2;
    SDL_RenderCopy( renderer, touch_joystick, NULL, &dstrect );

    // Draw current touch position (50% size of repeat delay range)
    dstrect.w = dstrect.h = dstrect.w/2;
    dstrect.x = finger_down_x + (finger_curr_x - finger_down_x)/2 - dstrect.w/2;
    dstrect.y = finger_down_y + (finger_curr_y - finger_down_y)/2 - dstrect.h/2;
    SDL_RenderCopy( renderer, touch_joystick, NULL, &dstrect );

}

float clmp( float value, float low, float high ) { return ( value < low ) ? low : ( ( value > high ) ? high : value ); }
float lerp(float t, float a, float b) { return (1.0f - t) * a + t * b; }

void update_finger_repeat_delay() {
    float delta_x = finger_curr_x - finger_down_x;
    float delta_y = finger_curr_y - finger_down_y;
    float dist = (float)sqrtf(delta_x*delta_x + delta_y*delta_y);
    float longest_window_edge = std::max(WindowWidth, WindowHeight);
    float t = clmp((dist - (get_option<float>("ANDROID_DEADZONE_RANGE")*longest_window_edge)) / std::max(0.01f, (get_option<float>("ANDROID_REPEAT_DELAY_RANGE"))*longest_window_edge), 0.0f, 1.0f);
    finger_repeat_delay = lerp(std::pow(t, get_option<float>("ANDROID_SENSITIVITY_POWER")), 
        (unsigned long)std::max(get_option<int>("ANDROID_REPEAT_DELAY_MIN"), get_option<int>("ANDROID_REPEAT_DELAY_MAX")), 
        (unsigned long)std::min(get_option<int>("ANDROID_REPEAT_DELAY_MIN"), get_option<int>("ANDROID_REPEAT_DELAY_MAX")));
}

// TODO: Is there a better way to detect when string entry is allowed?
// ANY_INPUT seems close but is abused by code everywhere.
// Had a look through and think I've got all the cases but can't be 100% sure.
bool is_string_input(input_context& ctx) {
    std::string& category = ctx.get_category();
    return category == "STRING_INPUT"
        || category == "HELP_KEYBINDINGS"
        || category == "NEW_CHAR_DESCRIPTION"
        || category == "WORLDGEN_CONFIRM_DIALOG";
}

// This function is triggered on finger up events, OR by a repeating timer for touch hold events.
void handle_finger_input(unsigned long ticks) {

    float delta_x = finger_curr_x - finger_down_x;
    float delta_y = finger_curr_y - finger_down_y;
    float dist = (float)sqrtf(delta_x*delta_x + delta_y*delta_y); // in pixel space
    bool handle_diagonals = touch_input_context.is_action_registered("LEFTUP");

    if (dist > (get_option<float>("ANDROID_DEADZONE_RANGE")*std::max(WindowWidth, WindowHeight))) {
        if (!handle_diagonals) {
            if (delta_x >= 0 && delta_y >= 0)
                last_input = input_event(delta_x > delta_y ? KEY_RIGHT : KEY_DOWN, CATA_INPUT_KEYBOARD);
            else if (delta_x < 0 && delta_y >= 0)
                last_input = input_event(-delta_x > delta_y ? KEY_LEFT : KEY_DOWN, CATA_INPUT_KEYBOARD);
            else if (delta_x >= 0 && delta_y < 0)
                last_input = input_event(delta_x > -delta_y ? KEY_RIGHT : KEY_UP, CATA_INPUT_KEYBOARD);
            else if (delta_x < 0 && delta_y < 0)
                last_input = input_event(-delta_x > -delta_y ? KEY_LEFT : KEY_UP, CATA_INPUT_KEYBOARD);
        }
        else {
            if (delta_x > 0) {
                if (std::abs(delta_y) < delta_x * 0.5f) {
                    // swipe right
                    last_input = input_event(KEY_RIGHT, CATA_INPUT_KEYBOARD);
                }
                else if (std::abs(delta_y) < delta_x * 2.0f) {
                    if (delta_y < 0) {
                        // swipe up-right
                        last_input = input_event(JOY_RIGHTUP, CATA_INPUT_GAMEPAD);
                    }
                    else {
                        // swipe down-right
                        last_input = input_event(JOY_RIGHTDOWN, CATA_INPUT_GAMEPAD);
                    }
                }
                else {
                    if (delta_y < 0) {
                        // swipe up
                        last_input = input_event(KEY_UP, CATA_INPUT_KEYBOARD);
                    }
                    else {
                        // swipe down
                        last_input = input_event(KEY_DOWN, CATA_INPUT_KEYBOARD);
                    }
                }
            }
            else {
                if (std::abs(delta_y) < -delta_x * 0.5f) {
                    // swipe left
                    last_input = input_event(KEY_LEFT, CATA_INPUT_KEYBOARD);
                }
                else if (std::abs(delta_y) < -delta_x * 2.0f) {
                    if (delta_y < 0) {
                        // swipe up-left
                        last_input = input_event(JOY_LEFTUP, CATA_INPUT_GAMEPAD);

                    }
                    else {
                        // swipe down-left
                        last_input = input_event(JOY_LEFTDOWN, CATA_INPUT_GAMEPAD);
                    }
                }
                else {
                    if (delta_y < 0) {
                        // swipe up
                        last_input = input_event(KEY_UP, CATA_INPUT_KEYBOARD);
                    }
                    else {
                        // swipe down
                        last_input = input_event(KEY_DOWN, CATA_INPUT_KEYBOARD);
                    }
                }
            }
        }
    }
    else {
        if (ticks - finger_down_time >= (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY")) {
            // Single tap (repeat) - held, so always treat this as a tap
            // We only allow repeats for waiting, not confirming in menus as that's a bit silly
            if (touch_input_context.get_category() == "DEFAULTMODE")
                last_input = input_event(choose_best_key_for_action("pause", touch_input_context.get_category()), CATA_INPUT_KEYBOARD);
        }
        else {
            if (last_tap_time > 0 && ticks - last_tap_time < (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY")) {
                // Double tap
                last_input = input_event(KEY_ESCAPE, CATA_INPUT_KEYBOARD);
                last_tap_time = 0;
            }
            else {
                // First tap detected, waiting to decide whether it's a single or a double tap input
                last_tap_time = ticks;
            }
        }
    }
}

void android_vibrate() {
    int vibration_ms = get_option<int>("ANDROID_VIBRATION");
    if (vibration_ms > 0) {
        JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
        jobject activity = (jobject)SDL_AndroidGetActivity();
        jclass clazz(env->GetObjectClass(activity));
        jmethodID method_id = env->GetMethodID(clazz, "vibrate", "(I)V");
        env->CallVoidMethod(activity, method_id, vibration_ms);
        env->DeleteLocalRef(activity);
        env->DeleteLocalRef(clazz);
    }    
}

void anroid_set_screen_orientation(int orientation) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    jclass clazz(env->GetObjectClass(activity));
    jmethodID method_id = env->GetMethodID(clazz, "setRequestedOrientation", "(I)V");
    env->CallVoidMethod(activity, method_id, orientation);
    env->DeleteLocalRef(activity);
    env->DeleteLocalRef(clazz);
}
#endif

//Check for any window messages (keypress, paint, mousemove, etc)
void CheckMessages()
{
    SDL_Event ev;
    bool quit = false;
    if(HandleDPad()) {
        return;
    }

#ifdef __ANDROID__
    if (visible_display_frame_dirty) {
       needupdate = true;
       visible_display_frame_dirty = false;
    }

    // Handle screen orientation changes
    if (needupdate) {
        // Ref: https://developer.android.com/reference/android/R.attr.html#screenOrientation
        static std::string screenOrientation;
        std::string newScreenOrientation = get_option<std::string>( "ANDROID_SCREEN_ORIENTATION" );
        if (newScreenOrientation != screenOrientation) {
            screenOrientation = newScreenOrientation;
            if (screenOrientation == "slSensor") {
                anroid_set_screen_orientation(-1); // SCREEN_ORIENTATION_UNSPECIFIED
            } else if (screenOrientation == "slPortrait") {
                anroid_set_screen_orientation(1); // SCREEN_ORIENTATION_PORTRAIT
            } else if (screenOrientation == "slLandscapeLeft") {
                anroid_set_screen_orientation(0); // SCREEN_ORIENTATION_LANDSCAPE
            } else if (screenOrientation == "slLandscapeRight") {
                anroid_set_screen_orientation(8); // SCREEN_ORIENTATION_REVERSE_LANDSCAPE.
            } 
        }        
    }

    unsigned long ticks = SDL_GetTicks();

    // Copy the current input context
    if (input_context::input_context_stack.size() > 0) {
        input_context* new_input_context = *--input_context::input_context_stack.end();
        if (new_input_context && *new_input_context != touch_input_context) {
            //LOGD("touch_input_context changed, copying...");
            touch_input_context = *new_input_context;
            needupdate = true;
        }
    }

    bool is_default_mode = touch_input_context.get_category() == "DEFAULTMODE";
    quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(touch_input_context.get_category())];

    // Don't do this logic if we already need an update, otherwise we're likely to overload the game with too much input on hold repeat events
    if (!needupdate) {

        // Check action weightings and auto-add any immediate-surrounding actions as quick shortcuts
        // This code is based heavily off action.cpp handle_action_menu() which puts common shortcuts at the top
        if (is_default_mode && get_option<bool>("ANDROID_SHORTCUT_AUTOADD")) {
            static int last_moves_since_last_save = -1;
            if (last_moves_since_last_save != g->moves_since_last_save) {
                //LOGD("last_moves_since_last_save: %d moves_since_last_save %d, refreshing shortcut actions...", last_moves_since_last_save, g->moves_since_last_save);
                last_moves_since_last_save = g->moves_since_last_save;

                // Actions to add
                std::set<action_id> actions;

                // Actions to remove - we only want to remove things that we're 100% sure won't be useful to players otherwise
                std::set<action_id> actions_remove;
                
                // Check if we're in a potential combat situation, if so, sort a few actions to the top.
                if( !g->u.get_hostile_creatures( 60 ).empty() ) {
                    // Only prioritize movement options if we're not driving.
                    if( !g->u.controlling_vehicle ) {
                        actions.insert(ACTION_TOGGLE_MOVE);
                    }
                    // Only prioritize fire weapon options if we're wielding a ranged weapon.
                    if( g->u.weapon.is_gun() || g->u.weapon.has_flag( "REACH_ATTACK" ) ) {
                        actions.insert(ACTION_FIRE);
                    }
                }

                // If we're already running, make it simple to toggle running to off.
                if( g->u.move_mode != "walk" ) {
                    actions.insert(ACTION_TOGGLE_MOVE);
                }

                // We're not already running or in combat, so remove toggle walk/run
                if (std::find(actions.begin(), actions.end(), ACTION_TOGGLE_MOVE) == actions.end()) {
                    actions_remove.insert(ACTION_TOGGLE_MOVE);
                }

                // Check if we can perform one of our actions on nearby terrain. If so,
                // display that action at the top of the list.
                for( int dx = -1; dx <= 1; dx++ ) {
                    for( int dy = -1; dy <= 1; dy++ ) {
                        int x = g->u.posx() + dx;
                        int y = g->u.posy() + dy;
                        int z = g->u.posz();
                        const tripoint pos( x, y, z );
        
                        // Check if we're near a vehicle, if so, vehicle controls should be top.
                        {
                            int veh_part = 0;
                            vehicle *veh = NULL;

                            veh = g->m.veh_at( pos, veh_part );
                            if( veh ) {
                                if (veh->part_with_feature(veh_part, "CONTROLS") >= 0)
                                    actions.insert(ACTION_CONTROL_VEHICLE);
                                int openablepart = veh->part_with_feature(veh_part, "OPENABLE");
                                if (openablepart >= 0 && veh->is_open(openablepart) && (dx != 0 || dy != 0)) // an open door adjacent to us
                                    actions.insert(ACTION_CLOSE);
                                int curtainpart = veh->part_with_feature(veh_part, "CURTAIN");
                                if (curtainpart >= 0 && veh->is_open(curtainpart) && (dx != 0 || dy != 0))
                                    actions.insert(ACTION_CLOSE);
                                if (dx == 0 && dy == 0) {
                                    int cargopart = veh->part_with_feature(veh_part, "CARGO");
                                    bool can_pickup = cargopart >= 0 && (!veh->get_items(cargopart).empty());
                                    if (can_pickup)
                                        actions.insert(ACTION_PICKUP);
                                }
                            }
                        }

                        if( dx != 0 || dy != 0 ) {
                            // Check for actions that work on nearby tiles
                            //if( can_interact_at( ACTION_OPEN, pos ) ) {
                                // don't bother with open since user can just walk into target
                            //}
                            if( can_interact_at( ACTION_CLOSE, pos ) ) {
                                actions.insert(ACTION_CLOSE);
                            }
                            if( can_interact_at( ACTION_EXAMINE, pos ) ) {
                                actions.insert(ACTION_EXAMINE);
                            }
                        } else {
                            // Check for actions that work on own tile only
                            if( can_interact_at( ACTION_BUTCHER, pos ) ) {
                                actions.insert(ACTION_BUTCHER);
                            }
                            else {
                                actions_remove.insert(ACTION_BUTCHER);
                            }
                            
                            if( can_interact_at( ACTION_MOVE_UP, pos ) ) {
                                actions.insert(ACTION_MOVE_UP);
                            }
                            else {
                                actions_remove.insert(ACTION_MOVE_UP);
                            }

                            if( can_interact_at( ACTION_MOVE_DOWN, pos ) ) {
                                actions.insert(ACTION_MOVE_DOWN);
                            }
                            else {
                                actions_remove.insert(ACTION_MOVE_DOWN);
                            }
                        }
                    }
                }

                // We're not near a vehicle, so remove control vehicle
                if (std::find(actions.begin(), actions.end(), ACTION_CONTROL_VEHICLE) == actions.end()) {
                    actions_remove.insert(ACTION_CONTROL_VEHICLE);
                }

                // We're not able to close anything nearby, so remove it
                if (std::find(actions.begin(), actions.end(), ACTION_CLOSE) == actions.end()) {
                    actions_remove.insert(ACTION_CLOSE);
                }

                // We're not able to examine anything nearby, so remove it
                if (std::find(actions.begin(), actions.end(), ACTION_EXAMINE) == actions.end()) {
                    actions_remove.insert(ACTION_EXAMINE);
                }

                // If we're standing on items, allow player to pick them up.
                if( g->m.has_items( g->u.pos() ) ) {
                    actions.insert(ACTION_PICKUP);
                }

                // We're not able to pickup anything, so remove it
                if (std::find(actions.begin(), actions.end(), ACTION_PICKUP) == actions.end()) {
                    actions_remove.insert(ACTION_PICKUP);
                }

                // Check if we can't move because of safe mode - if so, add ability to ignore
                if (g && !g->check_safe_mode_allowed(false)) {
                    actions.insert(ACTION_IGNORE_ENEMY);
                    actions.insert(ACTION_TOGGLE_SAFEMODE);
                }
                else {
                    actions_remove.insert(ACTION_IGNORE_ENEMY);
                    actions_remove.insert(ACTION_TOGGLE_SAFEMODE);
                }

                // Check if we're significantly hungry or thirsty - if so, add eat
                if (g->u.get_hunger() > 100 || g->u.get_thirst() > 40) {
                    actions.insert(ACTION_EAT);
                }

                // Check if we're dead tired - if so, add sleep
                if (g->u.get_fatigue() > DEAD_TIRED) {
                    actions.insert(ACTION_SLEEP);
                }

                for(const auto& action : actions) {
                    if (add_best_key_for_action_to_quick_shortcuts(action, touch_input_context.get_category(), !get_option<bool>("ANDROID_SHORTCUT_AUTOADD_FRONT")))
						needupdate = true;
                }

                size_t old_size = qsl.size();
                for(const auto& action_remove : actions_remove)
                    remove_action_from_quick_shortcuts(action_remove, touch_input_context.get_category());
                if (qsl.size() != old_size)
                    needupdate = true;
            }
        }

		if (remove_expired_actions_from_quick_shortcuts(touch_input_context.get_category()))
			needupdate = true;

        // Toggle quick shortcuts on/off
        if (ac_back_down_time > 0 && ticks - ac_back_down_time > (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY")) {
            if (!quick_shortcuts_toggle_handled) {
                quick_shortcuts_enabled = !quick_shortcuts_enabled;
                quick_shortcuts_toggle_handled = true;
                refresh_display();

                // Display an Android toast message
                {
                    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
                    jobject activity = (jobject)SDL_AndroidGetActivity();
                    jclass clazz(env->GetObjectClass(activity));
                    jstring toast_message = env->NewStringUTF(quick_shortcuts_enabled ? "Shortcuts enabled" : "Shortcuts disabled");
                    jmethodID method_id = env->GetMethodID(clazz, "toast", "(Ljava/lang/String;)V");
                    env->CallVoidMethod(activity, method_id, toast_message);
                    env->DeleteLocalRef(activity);
                    env->DeleteLocalRef(clazz);
                }
            }
        }

        // Handle repeating inputs from touch + holds
        if (!is_quick_shortcut_touch && !is_two_finger_touch && finger_down_time > 0 && ticks - finger_down_time > (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY")) {
            if (ticks - finger_repeat_time > finger_repeat_delay) {
                handle_finger_input(ticks);
                finger_repeat_time = ticks;
                return;
            }
        }

        // If we received a first tap and not another one within a certain period, this was a single tap, so trigger the input event
        if (!is_quick_shortcut_touch && !is_two_finger_touch && last_tap_time > 0 && ticks - last_tap_time >= (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY")) {
            // Single tap
            //LOGD("single tap, is_default_mode: %d category: %s", is_default_mode, touch_input_context.get_category().c_str());
            last_tap_time = ticks;
            last_input = input_event(is_default_mode ? choose_best_key_for_action("pause", touch_input_context.get_category()) : '\n', CATA_INPUT_KEYBOARD);
            last_tap_time = 0;
            return;
        }

        // ensure hint text pops up even if player doesn't move finger to trigger a FINGERMOTION event
        if (is_quick_shortcut_touch && finger_down_time > 0 && ticks - finger_down_time > (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY")) {
            needupdate = true;
        }
    }
#endif

    last_input = input_event();
    while(SDL_PollEvent(&ev)) {
        switch(ev.type) {
            case SDL_WINDOWEVENT:
                switch(ev.window.event) {
#ifdef __ANDROID__
                // SDL will send a focus lost event whenever the app loses focus (eg. lock screen, switch app focus etc.)
                // If we detect it and the game seems in a saveable state, try and do a quicksave. This is a bit dodgy
                // as the player could be ANYWHERE doing ANYTHING (a sub-menu, interacting with an NPC/computer etc.)
                // but it seems to work so far, and the alternative is the player losing their progress as the app is likely
                // to be destroyed pretty quickly when it goes out of focus due to memory usage.
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    if (world_generator && world_generator->active_world && g && g->uquit == QUIT_NO && get_option<bool>("ANDROID_QUICKSAVE")) 
                        g->quicksave();
                    break;
                // SDL sends a window size changed event whenever the screen rotates orientation
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    //LOGD("Window size changed: OldWindowWidth: %d OldWindowHeight: %d WindowWidth: %d WindowHeight: %d", WindowWidth, WindowHeight, ev.window.data1, ev.window.data2);
                    WindowWidth = ev.window.data1;
                    WindowHeight = ev.window.data2;
                    refresh_display();
                    needupdate = true;
                    break;
#endif
                case SDL_WINDOWEVENT_SHOWN:
                case SDL_WINDOWEVENT_EXPOSED:
                case SDL_WINDOWEVENT_RESTORED:
                    needupdate = true;
                    break;
                default:
                    break;
                }
            break;
            case SDL_KEYDOWN:
            {
#ifdef __ANDROID__
                // Toggle virtual keyboard with Android back button. For some reason I get double inputs, so ignore everything once it's already down.
                if (ev.key.keysym.sym == SDLK_AC_BACK && ac_back_down_time == 0) {
                    LOGD("SDLK_AC_BACK down");
                    ac_back_down_time = ticks;
                    quick_shortcuts_toggle_handled = false;
                }
#endif

                //hide mouse cursor on keyboard input
                if(get_option<std::string>( "HIDE_CURSOR" ) != "show" && SDL_ShowCursor(-1)) {
                    SDL_ShowCursor(SDL_DISABLE);
                }
                const Uint8 *keystate = SDL_GetKeyboardState(NULL);
                // manually handle Alt+F4 for older SDL lib, no big deal
                if( ev.key.keysym.sym == SDLK_F4
                && (keystate[SDL_SCANCODE_RALT] || keystate[SDL_SCANCODE_LALT]) ) {
                    quit = true;
                    break;
                }
                const long lc = sdl_keysym_to_curses(ev.key.keysym);
                if( lc <= 0 ) {
                    // a key we don't know in curses and won't handle.
                    break;
                } else if( add_alt_code( lc ) ) {
                    // key was handled
                } else {
                    last_input = input_event(lc, CATA_INPUT_KEYBOARD);
#ifdef __ANDROID__
                    if (!is_string_input(touch_input_context)) {
                        if (get_option<bool>("ANDROID_AUTO_KEYBOARD"))
                            SDL_StopTextInput();

                        // add a quick shortcut
                        if (!last_input.text.empty() || !inp_mngr.get_keyname(lc, CATA_INPUT_KEYBOARD).empty()) {
                            qsl.remove(last_input);
                            add_quick_shortcut(qsl, last_input, false, true);
                            refresh_display();
                            //for (std::list<input_event>::iterator it = qsl.begin(); it != qsl.end(); ++it)
                            //    LOGD("quick shortcuts for tic %s: %s", touch_input_context.get_category().c_str(), (*it).text.c_str());                                
                        }
                    }
                    else if (lc == '\n' || lc == KEY_ESCAPE) {
                        if (get_option<bool>("ANDROID_AUTO_KEYBOARD"))
                            SDL_StopTextInput();
                    }
#endif
                }
            }
            break;
            case SDL_KEYUP:
            {
#ifdef __ANDROID__
				// Toggle virtual keyboard with Android back button
                if (ev.key.keysym.sym == SDLK_AC_BACK) {
                    if (ticks - ac_back_down_time <= (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY")) {
                        if (SDL_IsTextInputActive())
                            SDL_StopTextInput();
                        else
                            SDL_StartTextInput();
                    }
                    ac_back_down_time = 0;
                }
#endif
                if( ev.key.keysym.sym == SDLK_LALT || ev.key.keysym.sym == SDLK_RALT ) {
                    int code = end_alt_code();
                    if( code ) {
                        last_input = input_event(code, CATA_INPUT_KEYBOARD);
                        last_input.text = utf32_to_utf8(code);
                    }
                }
            }
            break;
            case SDL_TEXTINPUT:
                if( !add_alt_code( *ev.text.text ) ) {
                    const char *c = ev.text.text;
                    int len = strlen(ev.text.text);
                    const unsigned lc = UTF8_getch( &c, &len );
                    last_input = input_event( lc, CATA_INPUT_KEYBOARD );
                    last_input.text = ev.text.text;

#ifdef __ANDROID__
                    if (!is_string_input(touch_input_context)) {
                        if (get_option<bool>("ANDROID_AUTO_KEYBOARD"))
                            SDL_StopTextInput();

                        quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(touch_input_context.get_category())];
                        qsl.remove(last_input);
                        add_quick_shortcut(qsl, last_input, false, true);
                        refresh_display();
                        //for (std::list<input_event>::iterator it = qsl.begin(); it != qsl.end(); ++it)
                        //    LOGD("quick shortcuts for tic %s: %s", touch_input_context.get_category().c_str(), (*it).text.c_str());
                    }
                    else if (lc == '\n' || lc == KEY_ESCAPE) {
                        if (get_option<bool>("ANDROID_AUTO_KEYBOARD"))
                            SDL_StopTextInput();
                    }
#endif
                }
            break;
            case SDL_JOYBUTTONDOWN:
                last_input = input_event(ev.jbutton.button, CATA_INPUT_KEYBOARD);
            break;
            case SDL_JOYAXISMOTION: // on gamepads, the axes are the analog sticks
                // TODO: somehow get the "digipad" values from the axes
            break;
#ifndef __ANDROID__
            // Disable mouse input on Android build, interferes with touch input
            case SDL_MOUSEMOTION:
                if (get_option<std::string>( "HIDE_CURSOR" ) == "show" || get_option<std::string>( "HIDE_CURSOR" ) == "hidekb") {
                    if (!SDL_ShowCursor(-1)) {
                        SDL_ShowCursor(SDL_ENABLE);
                    }

                    // Only monitor motion when cursor is visible
                    last_input = input_event(MOUSE_MOVE, CATA_INPUT_MOUSE);
                }
                break;

            case SDL_MOUSEBUTTONUP:
                switch (ev.button.button) {
                    case SDL_BUTTON_LEFT:
                        last_input = input_event(MOUSE_BUTTON_LEFT, CATA_INPUT_MOUSE);
                        break;
                    case SDL_BUTTON_RIGHT:
                        last_input = input_event(MOUSE_BUTTON_RIGHT, CATA_INPUT_MOUSE);
                        break;
                    }
                break;

            case SDL_MOUSEWHEEL:
                if(ev.wheel.y > 0) {
                    last_input = input_event(SCROLLWHEEL_UP, CATA_INPUT_MOUSE);
                } else if(ev.wheel.y < 0) {
                    last_input = input_event(SCROLLWHEEL_DOWN, CATA_INPUT_MOUSE);
                }
                break;
#endif

#ifdef __ANDROID__
              case SDL_FINGERMOTION:
                //LOGD("SDL_FINGERMOTION: id:%lld x:%f y:%f", ev.tfinger.fingerId, ev.tfinger.x, ev.tfinger.y);
                    if (ev.tfinger.fingerId == 0) {
                        if (!is_quick_shortcut_touch)
                            update_finger_repeat_delay();
                        needupdate = true; // ensure virtual joystick and quick shortcuts redraw as we interact
                        finger_curr_x = ev.tfinger.x * WindowWidth;
                        finger_curr_y = ev.tfinger.y * WindowHeight;

                        if (get_option<bool>("ANDROID_VIRTUAL_JOYSTICK_FOLLOW")) {
                            // If we've moved too far from joystick center, offset joystick center automatically
                            float delta_x = finger_curr_x - finger_down_x;
                            float delta_y = finger_curr_y - finger_down_y;
                            float dist = (float)sqrtf(delta_x*delta_x + delta_y*delta_y);
                            float max_dist = (get_option<float>("ANDROID_DEADZONE_RANGE") + get_option<float>("ANDROID_REPEAT_DELAY_RANGE")) * std::max(WindowWidth, WindowHeight);
                            if (dist > max_dist) {
                                float delta_ratio = (dist / max_dist) - 1.0f;
                                finger_down_x += delta_x * delta_ratio;
                                finger_down_y += delta_y * delta_ratio;
                            }                            
                        }

                    }
                    else if (ev.tfinger.fingerId == 1) {
                        second_finger_curr_x = ev.tfinger.x * WindowWidth;
                        second_finger_curr_y = ev.tfinger.y * WindowHeight;
                    }
                break;
              case SDL_FINGERDOWN:
                //LOGD("SDL_FINGERDOWN: id:%lld x:%f y:%f", ev.tfinger.fingerId, ev.tfinger.x, ev.tfinger.y);
                    if (ev.tfinger.fingerId == 0) {
                        finger_down_x = finger_curr_x = ev.tfinger.x * WindowWidth;
                        finger_down_y = finger_curr_y = ev.tfinger.y * WindowHeight;
                        finger_down_time = ticks;
                        finger_repeat_time = 0;
                        is_quick_shortcut_touch = get_quick_shortcut_under_finger() != NULL;
                        if (!is_quick_shortcut_touch)
                            update_finger_repeat_delay();
                        needupdate = true; // ensure virtual joystick and quick shortcuts redraw as we interact
                    } 
                    else if (ev.tfinger.fingerId == 1) {
                        if (!is_quick_shortcut_touch) {
                            second_finger_down_x = second_finger_curr_x = ev.tfinger.x * WindowWidth;
                            second_finger_down_y = second_finger_curr_y = ev.tfinger.y * WindowHeight;
                            is_two_finger_touch = true;
                        }
                    }
                break;
              case SDL_FINGERUP:
                //LOGD("SDL_FINGERUP: id:%lld x:%f y:%f", ev.tfinger.fingerId, ev.tfinger.x, ev.tfinger.y);
                if (ev.tfinger.fingerId == 0) {
                    finger_curr_x = ev.tfinger.x * WindowWidth;
                    finger_curr_y = ev.tfinger.y * WindowHeight;
                    if (is_quick_shortcut_touch) {
                        input_event* quick_shortcut = get_quick_shortcut_under_finger();
                        if (quick_shortcut) {
                            last_input = *quick_shortcut;
                            if (get_option<bool>("ANDROID_SHORTCUT_MOVE_FRONT")) {
                                quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(touch_input_context.get_category())];
                                reorder_quick_shortcut(qsl, quick_shortcut->get_first_input(), false);
                            }
                            quick_shortcut->shortcut_last_used_action_counter = g->get_user_action_counter();
                        }
                        else {
                            // Get the quick shortcut that was originally touched
                            quick_shortcut = get_quick_shortcut_under_finger(true);
                            if (quick_shortcut && 
                                ticks - finger_down_time <= (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY") &&
                                finger_curr_y < finger_down_y &&
                                finger_down_y - finger_curr_y > std::abs(finger_down_x - finger_curr_x))
                            {
                                // a flick up was detected, remove the quick shortcut!
                                quick_shortcuts_t& qsl = quick_shortcuts_map[get_quick_shortcut_name(touch_input_context.get_category())];
                                qsl.remove(*quick_shortcut);
                            }
                        }
                    }
                    else {
                        if (is_two_finger_touch) {
                            // handle zoom in/out
                            if (is_default_mode) {
                                float down_x = finger_down_x - second_finger_down_x;
                                float down_y = finger_down_y - second_finger_down_y;
                                float down_dist = (float)sqrtf(down_x*down_x + down_y*down_y);
                                float curr_x = finger_curr_x - second_finger_curr_x;
                                float curr_y = finger_curr_y - second_finger_curr_y;
                                float curr_dist = (float)sqrtf(curr_x*curr_x + curr_y*curr_y);
                                const float zoom_ratio = 0.9f;
                                if (curr_dist < down_dist * zoom_ratio) {
                                    // zoom out
                                    last_input = input_event('z', CATA_INPUT_KEYBOARD);
                                } else if (curr_dist > down_dist / zoom_ratio) {
                                    // zoom in
                                    last_input = input_event('Z', CATA_INPUT_KEYBOARD);
                                }                                 
                            }
                        }
                        else if (ticks - finger_down_time <= (unsigned long)get_option<int>("ANDROID_INITIAL_DELAY")) {
                            handle_finger_input(ticks);                        
                        }
                    }
                    second_finger_down_x = second_finger_curr_x = finger_down_x = finger_curr_x = -1.0f;
                    second_finger_down_y = second_finger_curr_y = finger_down_y = finger_curr_y = -1.0f;
                    is_two_finger_touch = false;
                    finger_down_time = 0;
                    finger_repeat_time = 0;
                    needupdate = true; // ensure virtual joystick and quick shortcuts are updated properly
                    refresh_display(); // as above, but actually redraw it now as well
                }
                else if (ev.tfinger.fingerId == 1) {
                    if (is_two_finger_touch) {
                    // on second finger release, just remember the x/y position so we can calculate delta once first finger is done
                    // is_two_finger_touch will be reset when first finger lifts (see above)
                    second_finger_curr_x = ev.tfinger.x * WindowWidth;
                    second_finger_curr_y = ev.tfinger.y * WindowHeight;                        
                    }
                }

                break;
#endif

            case SDL_QUIT:
                quit = true;
                break;
        }
    }
    if (needupdate) {
        try_sdl_update();
    }
    if(quit) {
        endwin();
        exit(0);
    }
}

// Check if text ends with suffix
static bool ends_with(const std::string &text, const std::string &suffix) {
    return text.length() >= suffix.length() &&
        strcasecmp(text.c_str() + text.length() - suffix.length(), suffix.c_str()) == 0;
}

//***********************************
//Psuedo-Curses Functions           *
//***********************************

static void font_folder_list(std::ofstream& fout, std::string path)
{
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir (path.c_str())) != NULL) {
        bool found = false;
        while (!found && (ent = readdir (dir)) != NULL) {
            if( 0 == strcmp( ent->d_name, "." ) ||
                0 == strcmp( ent->d_name, ".." ) ) {
                continue;
            }
            char path_last = *path.rbegin();
            std::string f;
            if (is_filesep(path_last)) {
                f = path + ent->d_name;
            } else {
                f = path + FILE_SEP + ent->d_name;
            }

            struct stat stat_buffer;
            if( stat( f.c_str(), &stat_buffer ) == -1 ) {
                continue;
            }
            if( S_ISDIR(stat_buffer.st_mode) ) {
                font_folder_list( fout, f );
                continue;
            }

            TTF_Font* fnt = TTF_OpenFont(f.c_str(), 12);
            if (fnt == NULL) {
                continue;
            }
            long nfaces = 0;
            nfaces = TTF_FontFaces(fnt);
            TTF_CloseFont(fnt);
            fnt = NULL;

            for(long i = 0; i < nfaces; i++) {
                fnt = TTF_OpenFontIndex(f.c_str(), 12, i);
                if (fnt == NULL) {
                    continue;
                }

                // Add font family
                char *fami = TTF_FontFaceFamilyName(fnt);
                if (fami != NULL) {
                    fout << fami;
                } else {
                    TTF_CloseFont(fnt);
                    continue;
                }

                // Add font style
                char *style = TTF_FontFaceStyleName(fnt);
                bool isbitmap = ends_with(f, ".fon");
                if (style != NULL && !isbitmap && strcasecmp(style, "Regular") != 0) {
                    fout << " " << style;
                }
                if (isbitmap) {
                    std::set<std::string>::iterator it;
                    it = bitmap_fonts.find(std::string(fami));
                    if (it == bitmap_fonts.end()) {
                        // First appearance of this font family
                        bitmap_fonts.insert(fami);
                    } else { // Font in set. Add filename to family string
                        size_t start = f.find_last_of("/\\");
                        size_t end = f.find_last_of(".");
                        if (start != std::string::npos && end != std::string::npos) {
                            fout << " [" << f.substr(start + 1, end - start - 1) + "]";
                        } else {
                            dbg( D_INFO ) << "Skipping wrong font file: \"" << f << "\"";
                        }
                    }
                }
                fout << std::endl;

                // Add filename and font index
                fout << f << std::endl;
                fout << i << std::endl;

                TTF_CloseFont(fnt);
                fnt = NULL;

                // We use only 1 style in bitmap fonts.
                if (isbitmap) {
                    break;
                }
            }
        }
        closedir (dir);
    }
}

static void save_font_list()
{
    std::ofstream fout(FILENAMES["fontlist"].c_str(), std::ios_base::trunc);

    font_folder_list(fout, FILENAMES["fontdir"]);

#if (defined _WIN32 || defined WINDOWS)
    char buf[256];
    GetSystemWindowsDirectory(buf, 256);
    strcat(buf, "\\fonts");
    font_folder_list(fout, buf);
#elif (defined _APPLE_ && defined _MACH_)
    /*
    // Well I don't know how osx actually works ....
    font_folder_list(fout, "/System/Library/Fonts");
    font_folder_list(fout, "/Library/Fonts");

    wordexp_t exp;
    wordexp("~/Library/Fonts", &exp, 0);
    font_folder_list(fout, exp.we_wordv[0]);
    wordfree(&exp);*/
#else // Other POSIX-ish systems
    font_folder_list(fout, "/usr/share/fonts");
    font_folder_list(fout, "/usr/local/share/fonts");
    char *home;
    if( ( home = getenv( "HOME" ) ) ) {
        std::string userfontdir = home;
        userfontdir += "/.fonts";
        font_folder_list( fout, userfontdir );
    }
#endif

    bitmap_fonts.clear();

    fout << "end of list" << std::endl;
}

static std::string find_system_font(std::string name, int& faceIndex)
{
    const std::string fontlist_path = FILENAMES["fontlist"];
    std::ifstream fin(fontlist_path.c_str());
    if ( !fin.is_open() ) {
        // Try opening the fontlist at the old location.
        fin.open(FILENAMES["legacy_fontlist"].c_str());
        if( !fin.is_open() ) {
            dbg( D_INFO ) << "Generating fontlist";
            assure_dir_exist(FILENAMES["config_dir"]);
            save_font_list();
            fin.open(fontlist_path.c_str());
            if( !fin ) {
                dbg( D_ERROR ) << "Can't open or create fontlist file " << fontlist_path;
                return "";
            }
        } else {
            // Write out fontlist to the new location.
            save_font_list();
        }
    }
    if ( fin.is_open() ) {
        std::string fname;
        std::string fpath;
        std::string iline;
        int index = 0;
        do {
            getline(fin, fname);
            if (fname == "end of list") break;
            getline(fin, fpath);
            getline(fin, iline);
            index = atoi(iline.c_str());
            if (0 == strcasecmp(fname.c_str(), name.c_str())) {
                faceIndex = index;
                return fpath;
            }
        } while (!fin.eof());
    }

    return "";
}

// bitmap font font size test
// return face index that has this size or below
static int test_face_size(std::string f, int size, int faceIndex)
{
    TTF_Font* fnt = TTF_OpenFontIndex(f.c_str(), size, faceIndex);
    if(fnt != NULL) {
        char* style = TTF_FontFaceStyleName(fnt);
        if(style != NULL) {
            int faces = TTF_FontFaces(fnt);
            bool found = false;
            for(int i = faces - 1; i >= 0 && !found; i--) {
                TTF_Font* tf = TTF_OpenFontIndex(f.c_str(), size, i);
                char* ts = NULL;
                if(NULL != tf) {
                   if( NULL != (ts = TTF_FontFaceStyleName(tf))) {
                       if(0 == strcasecmp(ts, style) && TTF_FontHeight(tf) <= size) {
                           faceIndex = i;
                           found = true;
                       }
                   }
                   TTF_CloseFont(tf);
                   tf = NULL;
                }
            }
        }
        TTF_CloseFont(fnt);
        fnt = NULL;
    }

    return faceIndex;
}

// Calculates the new width of the window, given the number of columns.
int projected_window_width(int)
{
    return get_option<int>( "TERMINAL_X" ) * fontwidth;
}

// Calculates the new height of the window, given the number of rows.
int projected_window_height(int)
{
    return get_option<int>( "TERMINAL_Y" ) * fontheight;
}

//Basic Init, create the font, backbuffer, etc
WINDOW *curses_init(void)
{
    last_input = input_event();
    inputdelay = -1;

    std::string typeface, map_typeface, overmap_typeface;
    int fontsize = 8;
    int map_fontwidth = 8;
    int map_fontheight = 16;
    int map_fontsize = 8;
    int overmap_fontwidth = 8;
    int overmap_fontheight = 16;
    int overmap_fontsize = 8;

    std::ifstream jsonstream(FILENAMES["fontdata"].c_str(), std::ifstream::binary);
    if (jsonstream.good()) {
        JsonIn json(jsonstream);
        JsonObject config = json.get_object();
        fontblending = config.get_bool("fontblending", fontblending);
        fontwidth = config.get_int("fontwidth", fontwidth);
        fontheight = config.get_int("fontheight", fontheight);
        fontsize = config.get_int("fontsize", fontsize);
        typeface = config.get_string("typeface", typeface);
        map_fontwidth = config.get_int("map_fontwidth", fontwidth);
        map_fontheight = config.get_int("map_fontheight", fontheight);
        map_fontsize = config.get_int("map_fontsize", fontsize);
        map_typeface = config.get_string("map_typeface", typeface);
        overmap_fontwidth = config.get_int("overmap_fontwidth", fontwidth);
        overmap_fontheight = config.get_int("overmap_fontheight", fontheight);
        overmap_fontsize = config.get_int("overmap_fontsize", fontsize);
        overmap_typeface = config.get_string("overmap_typeface", typeface);
        jsonstream.close();
    } else { // User fontdata is missed. Try to load legacy fontdata.
        std::ifstream InStream(FILENAMES["legacy_fontdata"].c_str(), std::ifstream::binary);
        if(InStream.good()) {
            JsonIn jIn(InStream);
            JsonObject config = jIn.get_object();
            fontblending = config.get_bool("fontblending", fontblending);
            fontwidth = config.get_int("fontwidth", fontwidth);
            fontheight = config.get_int("fontheight", fontheight);
            fontsize = config.get_int("fontsize", fontsize);
            typeface = config.get_string("typeface", typeface);
            map_fontwidth = config.get_int("map_fontwidth", fontwidth);
            map_fontheight = config.get_int("map_fontheight", fontheight);
            map_fontsize = config.get_int("map_fontsize", fontsize);
            map_typeface = config.get_string("map_typeface", typeface);
            overmap_fontwidth = config.get_int("overmap_fontwidth", fontwidth);
            overmap_fontheight = config.get_int("overmap_fontheight", fontheight);
            overmap_fontsize = config.get_int("overmap_fontsize", fontsize);
            overmap_typeface = config.get_string("overmap_typeface", typeface);
            InStream.close();
            // Save legacy as user fontdata.
            assure_dir_exist(FILENAMES["config_dir"]);
            std::ofstream OutStream(FILENAMES["fontdata"].c_str(), std::ofstream::binary);
            if(!OutStream.good()) {
                dbg(D_ERROR) << "Can't save user fontdata file.\n" <<
                    "Check permissions for: " << FILENAMES["fontdata"];
                return NULL;
            }
            JsonOut jOut(OutStream, true); // pretty-print
            jOut.start_object();
            jOut.member("fontblending", fontblending);
            jOut.member("fontwidth", fontwidth);
            jOut.member("fontheight", fontheight);
            jOut.member("fontsize", fontsize);
            jOut.member("typeface", typeface);
            jOut.member("map_fontwidth", map_fontwidth);
            jOut.member("map_fontheight", map_fontheight);
            jOut.member("map_fontsize", map_fontsize);
            jOut.member("map_typeface", map_typeface);
            jOut.member("overmap_fontwidth", overmap_fontwidth);
            jOut.member("overmap_fontheight", overmap_fontheight);
            jOut.member("overmap_fontsize", overmap_fontsize);
            jOut.member("overmap_typeface", overmap_typeface);
            jOut.end_object();
            OutStream << "\n";
            OutStream.close();
        } else {
            dbg(D_ERROR) << "Can't load fontdata files.\n" << "Check permissions for:\n" <<
                FILENAMES["legacy_fontdata"] << "\n" << FILENAMES["fontdata"];
            return NULL;
        }
    }

    if(!InitSDL()) {
        return NULL;
    }

    find_videodisplays();

    TERMINAL_WIDTH = get_option<int>( "TERMINAL_X" );
    TERMINAL_HEIGHT = get_option<int>( "TERMINAL_Y" );

    if(!WinCreate()) {
        return NULL;
    }

    dbg( D_INFO ) << "Initializing SDL Tiles context";
    tilecontext.reset(new cata_tiles(renderer));
    try {
        tilecontext->init();
        dbg( D_INFO ) << "Tiles initialized successfully.";
    } catch( const std::exception &err ) {
        dbg( D_ERROR ) << "failed to initialize tile: " << err.what();
        // use_tiles is the cached value of the USE_TILES option.
        // most (all?) code refers to this to see if cata_tiles should be used.
        // Setting it to false disables this from getting used.
        use_tiles = false;
    }

    init_colors();

    // initialize sound set
    load_soundset();

    // Reset the font pointer
    font = Font::load_font(typeface, fontsize, fontwidth, fontheight);
    if( !font ) {
        return NULL;
    }
    map_font = Font::load_font(map_typeface, map_fontsize, map_fontwidth, map_fontheight);
    overmap_font = Font::load_font( overmap_typeface, overmap_fontsize,
                                    overmap_fontwidth, overmap_fontheight );
    mainwin = newwin(get_terminal_height(), get_terminal_width(),0,0);
    return mainwin;   //create the 'stdscr' window and return its ref
}

std::unique_ptr<Font> Font::load_font(const std::string &typeface, int fontsize, int fontwidth, int fontheight)
{
    if (ends_with(typeface, ".bmp") || ends_with(typeface, ".png")) {
        // Seems to be an image file, not a font.
        // Try to load as bitmap font.
        std::unique_ptr<BitmapFont> bm_font( new BitmapFont(fontwidth, fontheight) );
        try {
            bm_font->load_font(FILENAMES["fontdir"] + typeface);
            // It worked, tell the world to use bitmap_font.
            return std::unique_ptr<Font>( std::move( bm_font ) );
        } catch(std::exception &err) {
            dbg( D_ERROR ) << "Failed to load " << typeface << ": " << err.what();
            // Continue to load as truetype font
        }
    }
    // Not loaded as bitmap font (or it failed), try to load as truetype
    std::unique_ptr<CachedTTFFont> ttf_font( new CachedTTFFont(fontwidth, fontheight) );
    try {
        ttf_font->load_font(typeface, fontsize);
        // It worked, tell the world to use cached_ttf_font
        return std::unique_ptr<Font>( std::move( ttf_font ) );
    } catch(std::exception &err) {
        dbg( D_ERROR ) << "Failed to load " << typeface << ": " << err.what();
    }
    return nullptr;
}

//Ends the terminal, destroy everything
int curses_destroy(void)
{
    tilecontext.reset();
    font.reset();
    map_font.reset();
    overmap_font.reset();
    WinDestroy();
    return 1;
}

//copied from gdi version and don't bother to rename it
inline SDL_Color BGR(int b, int g, int r)
{
    SDL_Color result;
    result.b=b;    //Blue
    result.g=g;    //Green
    result.r=r;    //Red
    //result.a=0;//The Alpha, isnt used, so just set it to 0
    return result;
}

void load_colors( JsonObject &jsobj )
{
    JsonArray jsarr;
    for( size_t c = 0; c < main_color_names.size(); c++ ) {
        const std::string &color = main_color_names[c];
        auto &bgr = consolecolors[color];
        jsarr = jsobj.get_array( color );
        bgr.resize( 3 );
        // Strange ordering, isn't it? Entries in consolecolors are BGR,
        // the json contains them as RGB.
        bgr[0] = jsarr.get_int( 2 );
        bgr[1] = jsarr.get_int( 1 );
        bgr[2] = jsarr.get_int( 0 );
    }
}

// translate color entry in consolecolors to SDL_Color
inline SDL_Color ccolor( const std::string &color )
{
    const auto it = consolecolors.find( color );
    if( it == consolecolors.end() ) {
        dbg( D_ERROR ) << "requested non-existing color " << color << "\n";
        return SDL_Color { 0, 0, 0, 0 };
    }
    return BGR( it->second[0], it->second[1], it->second[2] );
}

// This function mimics the ncurses interface. It must not throw.
// Instead it should return ERR or OK, see man curs_color
int curses_start_color( void )
{
    const std::string path = FILENAMES["colors"];
    std::ifstream colorfile( path.c_str(), std::ifstream::in | std::ifstream::binary );
    try {
        JsonIn jsin( colorfile );
        // Manually load the colordef object because the json handler isn't loaded yet.
        jsin.start_array();
        while( !jsin.end_array() ) {
            JsonObject jo = jsin.get_object();
            load_colors( jo );
            jo.finish();
        }
    } catch( const JsonError &e ) {
        dbg( D_ERROR ) << "Failed to load color definitions from " << path << ": " << e;
        return ERR;
    }
    for( size_t c = 0; c < main_color_names.size(); c++ ) {
        windowsPalette[c]  = ccolor( main_color_names[c] );
    }
    return OK;
}

void input_manager::set_timeout( const int t )
{
    input_timeout = t;
    inputdelay = t;
}

extern WINDOW *mainwin;

#ifdef __ANDROID__
// Big dirty hack to ensure android intro message appears
bool is_android_intro_message = false;
#endif

// This is how we're actually going to handle input events, SDL getch
// is simply a wrapper around this.
input_event input_manager::get_input_event(WINDOW *win) {
    previously_pressed_key = 0;
    // standards note: getch is sometimes required to call refresh
    // see, e.g., http://linux.die.net/man/3/getch
    // so although it's non-obvious, that refresh() call (and maybe InvalidateRect?) IS supposed to be there

#ifdef __ANDROID__
    // BUGFIX for experimental - don't force mainwin if NULL win was passed in, otherwise this ruins the Android intro message.
    if (!is_android_intro_message)
        if(win == NULL) win = mainwin;
#endif

    wrefresh(win);

    if (inputdelay < 0)
    {
        do
        {
            CheckMessages();
            if (last_input.type != CATA_INPUT_ERROR) break;
            SDL_Delay(1);
        }
        while (last_input.type == CATA_INPUT_ERROR);
    }
    else if (inputdelay > 0)
    {
        unsigned long starttime=SDL_GetTicks();
        unsigned long endtime;
        bool timedout = false;
        do
        {
            CheckMessages();
            endtime=SDL_GetTicks();
            if (last_input.type != CATA_INPUT_ERROR) break;
            SDL_Delay(1);
            timedout = endtime >= starttime + inputdelay;
            if (timedout) {
                last_input.type = CATA_INPUT_TIMEOUT;
            }
        }
        while (!timedout);
    }
    else
    {
        CheckMessages();
    }

    if (last_input.type == CATA_INPUT_MOUSE) {
        SDL_GetMouseState(&last_input.mouse_x, &last_input.mouse_y);
    } else if (last_input.type == CATA_INPUT_KEYBOARD) {
        previously_pressed_key = last_input.get_first_input();
#ifdef __ANDROID__
        android_vibrate();
#endif
    }
#ifdef __ANDROID__
    else if (last_input.type == CATA_INPUT_GAMEPAD) {
        android_vibrate();
    }
#endif

    return last_input;
}

bool gamepad_available() {
    return joystick != NULL;
}

void rescale_tileset(int size) {
    tilecontext->set_draw_scale(size);
    g->init_ui();
    ClearScreen();
}

bool input_context::get_coordinates(WINDOW* capture_win, int& x, int& y) {
    if(!coordinate_input_received) {
        return false;
    }

    if (!capture_win) {
        capture_win = g->w_terrain;
    }

    // this contains the font dimensions of the capture_win,
    // not necessarily the global standard font dimensions.
    int fw = fontwidth;
    int fh = fontheight;
    // tiles might have different dimensions than standard font
    if (use_tiles && capture_win == g->w_terrain) {
        fw = tilecontext->get_tile_width();
        fh = tilecontext->get_tile_height();
        // add_msg( m_info, "tile map fw %d fh %d", fw, fh);
    } else if (map_font && capture_win == g->w_terrain) {
        // map font (if any) might differ from standard font
        fw = map_font->fontwidth;
        fh = map_font->fontheight;
    }

    // Translate mouse coords to map coords based on tile size,
    // the window position is *always* in standard font dimensions!
    const int win_left = capture_win->x * fontwidth;
    const int win_top = capture_win->y * fontheight;
    // But the size of the window is in the font dimensions of the window.
    const int win_right = win_left + (capture_win->width * fw);
    const int win_bottom = win_top + (capture_win->height * fh);
    // add_msg( m_info, "win_ left %d top %d right %d bottom %d", win_left,win_top,win_right,win_bottom);
    // add_msg( m_info, "coordinate_ x %d y %d", coordinate_x, coordinate_y);
    // Check if click is within bounds of the window we care about
    if( coordinate_x < win_left || coordinate_x > win_right ||
        coordinate_y < win_top || coordinate_y > win_bottom ) {
        // add_msg( m_info, "out of bounds");
        return false;
    }

    if ( tile_iso && use_tiles ) {
        const int screen_column = round( (float) ( coordinate_x - win_left - (( win_right - win_left ) / 2 + win_left ) ) / ( fw / 2 ) );
        const int screen_row = round( (float) ( coordinate_y - win_top - ( win_bottom - win_top ) / 2 + win_top ) / ( fw / 4 ) );
        const int selected_x = ( screen_column - screen_row ) / 2;
        const int selected_y = ( screen_row + screen_column ) / 2;
        x = g->ter_view_x + selected_x;
        y = g->ter_view_y + selected_y;
    } else {
        const int selected_column = (coordinate_x - win_left) / fw;
        const int selected_row = (coordinate_y - win_top) / fh;

        x = g->ter_view_x - ((capture_win->width / 2) - selected_column);
        y = g->ter_view_y - ((capture_win->height / 2) - selected_row);
    }

    return true;
}

int get_terminal_width() {
    return TERMINAL_WIDTH;
}

int get_terminal_height() {
    return TERMINAL_HEIGHT;
}

BitmapFont::BitmapFont(int w, int h)
: Font(w, h)
{
    memset(ascii, 0x00, sizeof(ascii));
}

BitmapFont::~BitmapFont()
{
    clear();
}

void BitmapFont::clear()
{
    for (size_t a = 0; a < 16; a++) {
        if (ascii[a] != NULL) {
            SDL_DestroyTexture(ascii[a]);
            ascii[a] = NULL;
        }
    }
}

void BitmapFont::load_font(const std::string &typeface)
{
    clear();
    dbg( D_INFO ) << "Loading bitmap font [" + typeface + "]." ;
    SDL_Surface *asciiload = IMG_Load(typeface.c_str());
    if (asciiload == NULL) {
        throw std::runtime_error(IMG_GetError());
    }
    if (asciiload->w * asciiload->h < (fontwidth * fontheight * 256)) {
        SDL_FreeSurface(asciiload);
        throw std::runtime_error("bitmap for font is to small");
    }
    Uint32 key = SDL_MapRGB(asciiload->format, 0xFF, 0, 0xFF);
    SDL_SetColorKey(asciiload,SDL_TRUE,key);
    SDL_Surface *ascii_surf[16];
    ascii_surf[0] = SDL_ConvertSurface(asciiload,format,0);
    SDL_SetSurfaceRLE(ascii_surf[0], true);
    SDL_FreeSurface(asciiload);

    for (size_t a = 1; a < 16; ++a) {
        ascii_surf[a] = SDL_ConvertSurface(ascii_surf[0],format,0);
        SDL_SetSurfaceRLE(ascii_surf[a], true);
    }

    for (size_t a = 0; a < 16 - 1; ++a) {
        SDL_LockSurface(ascii_surf[a]);
        int size = ascii_surf[a]->h * ascii_surf[a]->w;
        Uint32 *pixels = (Uint32 *)ascii_surf[a]->pixels;
        Uint32 color = (windowsPalette[a].r << 16) | (windowsPalette[a].g << 8) | windowsPalette[a].b;
        for(int i = 0; i < size; i++) {
            if(pixels[i] == 0xFFFFFF) {
                pixels[i] = color;
            }
        }
        SDL_UnlockSurface(ascii_surf[a]);
    }
    tilewidth = ascii_surf[0]->w / fontwidth;

    //convert ascii_surf to SDL_Texture
    for(int a = 0; a < 16; ++a) {
        ascii[a] = SDL_CreateTextureFromSurface(renderer,ascii_surf[a]);
        SDL_FreeSurface(ascii_surf[a]);
    }
}

void BitmapFont::draw_ascii_lines(unsigned char line_id, int drawx, int drawy, int FG) const
{
    BitmapFont *t = const_cast<BitmapFont*>(this);
    switch (line_id) {
        case LINE_OXOX_C://box bottom/top side (horizontal line)
            t->OutputChar(0xcd, drawx, drawy, FG);
            break;
        case LINE_XOXO_C://box left/right side (vertical line)
            t->OutputChar(0xba, drawx, drawy, FG);
            break;
        case LINE_OXXO_C://box top left
            t->OutputChar(0xc9, drawx, drawy, FG);
            break;
        case LINE_OOXX_C://box top right
            t->OutputChar(0xbb, drawx, drawy, FG);
            break;
        case LINE_XOOX_C://box bottom right
            t->OutputChar(0xbc, drawx, drawy, FG);
            break;
        case LINE_XXOO_C://box bottom left
            t->OutputChar(0xc8, drawx, drawy, FG);
            break;
        case LINE_XXOX_C://box bottom north T (left, right, up)
            t->OutputChar(0xca, drawx, drawy, FG);
            break;
        case LINE_XXXO_C://box bottom east T (up, right, down)
            t->OutputChar(0xcc, drawx, drawy, FG);
            break;
        case LINE_OXXX_C://box bottom south T (left, right, down)
            t->OutputChar(0xcb, drawx, drawy, FG);
            break;
        case LINE_XXXX_C://box X (left down up right)
            t->OutputChar(0xce, drawx, drawy, FG);
            break;
        case LINE_XOXX_C://box bottom east T (left, down, up)
            t->OutputChar(0xb9, drawx, drawy, FG);
            break;
        default:
            break;
    }
}




CachedTTFFont::CachedTTFFont(int w, int h)
: Font(w, h)
, font(NULL)
{
}

CachedTTFFont::~CachedTTFFont()
{
    clear();
}

void CachedTTFFont::clear()
{
    if (font != NULL) {
        TTF_CloseFont(font);
        font = NULL;
    }
    for( auto &a : glyph_cache_map ) {
        if( a.second.texture ) {
            SDL_DestroyTexture( a.second.texture );
        }
    }
    glyph_cache_map.clear();
}

void CachedTTFFont::load_font(std::string typeface, int fontsize)
{
    clear();
    int faceIndex = 0;
    const std::string sysfnt = find_system_font(typeface, faceIndex);
    if (!sysfnt.empty()) {
        typeface = sysfnt;
        dbg( D_INFO ) << "Using font [" + typeface + "]." ;
    }
    //make fontdata compatible with wincurse
    if(!file_exist(typeface)) {
        faceIndex = 0;
        typeface = FILENAMES["fontdir"] + typeface + ".ttf";
        dbg( D_INFO ) << "Using compatible font [" + typeface + "]." ;
    }
    //different default font with wincurse
    if(!file_exist(typeface)) {
        faceIndex = 0;
        typeface = FILENAMES["fontdir"] + "fixedsys.ttf";
        dbg( D_INFO ) << "Using fallback font [" + typeface + "]." ;
    }
    dbg( D_INFO ) << "Loading truetype font [" + typeface + "]." ;
    if(fontsize <= 0) {
        fontsize = fontheight - 1;
    }
    // SDL_ttf handles bitmap fonts size incorrectly
    if( typeface.length() > 4 &&
        strcasecmp(typeface.substr(typeface.length() - 4).c_str(), ".fon") == 0 ) {
        faceIndex = test_face_size(typeface, fontsize, faceIndex);
    }
    font = TTF_OpenFontIndex(typeface.c_str(), fontsize, faceIndex);
    if (font == NULL) {
        throw std::runtime_error(TTF_GetError());
    }
    TTF_SetFontStyle(font, TTF_STYLE_NORMAL);
}

int map_font_width() {
    if (use_tiles && tilecontext ) {
        return tilecontext->get_tile_width();
    }
    return (map_font ? map_font : font)->fontwidth;
}

int map_font_height() {
    if (use_tiles && tilecontext ) {
        return tilecontext->get_tile_height();
    }
    return (map_font ? map_font : font)->fontheight;
}

int overmap_font_width() {
    return (overmap_font ? overmap_font : font)->fontwidth;
}

int overmap_font_height() {
    return (overmap_font ? overmap_font : font)->fontheight;
}

void to_map_font_dimension(int &w, int &h) {
    w = (w * fontwidth) / map_font_width();
    h = (h * fontheight) / map_font_height();
}

void from_map_font_dimension(int &w, int &h) {
    w = (w * map_font_width() + fontwidth - 1) / fontwidth;
    h = (h * map_font_height() + fontheight - 1) / fontheight;
}

void to_overmap_font_dimension(int &w, int &h) {
    w = (w * fontwidth) / overmap_font_width();
    h = (h * fontheight) / overmap_font_height();
}

bool is_draw_tiles_mode() {
    return use_tiles;
}

SDL_Color cursesColorToSDL(int color) {
    // Extract the color pair ID.
    int pair = (color & 0x03fe0000) >> 17;
    return windowsPalette[colorpairs[pair].FG];
}

#ifdef SDL_SOUND

void musicFinished();

void play_music_file(std::string filename, int volume) {
    const std::string path = ( current_soundpack_path + "/" + filename );
    current_music = Mix_LoadMUS(path.c_str());
    if( current_music == nullptr ) {
        dbg( D_ERROR ) << "Failed to load audio file " << path << ": " << Mix_GetError();
        return;
    }
    Mix_VolumeMusic(volume * get_option<int>( "MUSIC_VOLUME" ) / 100);
    if( Mix_PlayMusic( current_music, 0 ) != 0 ) {
        dbg( D_ERROR ) << "Starting playlist " << path << " failed: " << Mix_GetError();
        return;
    }
    Mix_HookMusicFinished(musicFinished);
}

/** Callback called when we finish playing music. */
void musicFinished() {
    Mix_HaltMusic();
    Mix_FreeMusic(current_music);
    current_music = NULL;

    const auto iter = playlists.find( current_playlist );
    if( iter == playlists.end() ) {
        return;
    }
    const music_playlist &list = iter->second;
    if( list.entries.empty() ) {
        return;
    }

    // Load the next file to play.
    absolute_playlist_at++;

    // Wrap around if we reached the end of the playlist.
    if( absolute_playlist_at >= list.entries.size() ) {
        absolute_playlist_at = 0;
    }

    current_playlist_at = playlist_indexes.at( absolute_playlist_at );

    const auto &next = list.entries[current_playlist_at];
    play_music_file( next.file, next.volume );
}
#endif

void play_music(std::string playlist) {
#ifdef SDL_SOUND
    const auto iter = playlists.find( playlist );
    if( iter == playlists.end() ) {
        return;
    }
    const music_playlist &list = iter->second;
    if( list.entries.empty() ) {
        return;
    }

    // Don't interrupt playlist that's already playing.
    if(playlist == current_playlist) {
        return;
    }

    for( size_t i = 0; i < list.entries.size(); i++ ) {
        playlist_indexes.push_back( i );
    }
    if( list.shuffle ) {
        std::random_shuffle( playlist_indexes.begin(), playlist_indexes.end() );
    }

    current_playlist = playlist;
    current_playlist_at = playlist_indexes.at( absolute_playlist_at );

    const auto &next = list.entries[current_playlist_at];
    play_music_file( next.file, next.volume );
#else
    (void)playlist;
#endif
}

#ifdef SDL_SOUND
void sfx::load_sound_effects( JsonObject &jsobj ) {
    const id_and_variant key( jsobj.get_string( "id" ), jsobj.get_string( "variant", "default" ) );
    const int volume = jsobj.get_int( "volume", 100 );
    auto &effects = sound_effects_p[key];

    JsonArray jsarr = jsobj.get_array( "files" );
    while( jsarr.has_more() ) {
        sound_effect new_sound_effect;
        const std::string file = jsarr.next_string();
        std::string path = ( current_soundpack_path + "/" + file );
        new_sound_effect.chunk.reset( Mix_LoadWAV( path.c_str() ) );
        if( !new_sound_effect.chunk ) {
            dbg( D_ERROR ) << "Failed to load audio file " << path << ": " << Mix_GetError();
            continue; // don't want empty chunks in the map
        }
        new_sound_effect.volume = volume;

        effects.push_back( std::move( new_sound_effect ) );
    }
}

void sfx::load_playlist( JsonObject &jsobj )
{
    JsonArray jarr = jsobj.get_array( "playlists" );
    while( jarr.has_more() ) {
        JsonObject playlist = jarr.next_object();

        const std::string playlist_id = playlist.get_string( "id" );
        music_playlist playlist_to_load;
        playlist_to_load.shuffle = playlist.get_bool( "shuffle", false );

        JsonArray files = playlist.get_array( "files" );
        while( files.has_more() ) {
            JsonObject entry = files.next_object();
            const music_playlist::entry e{ entry.get_string( "file" ),  entry.get_int( "volume" ) };
            playlist_to_load.entries.push_back( e );
        }

        playlists[playlist_id] = std::move( playlist_to_load );
    }
}

// Returns a random sound effect matching given id and variant or `nullptr` if there is no
// matching sound effect.
const sound_effect* find_random_effect( const id_and_variant &id_variants_pair )
{
    const auto iter = sound_effects_p.find( id_variants_pair );
    if( iter == sound_effects_p.end() ) {
        return nullptr;
    }
    const auto &vector = iter->second;
    if( vector.empty() ) {
        return nullptr;
    }
    return &vector[rng( 0, vector.size() - 1 )];
}
// Same as above, but with fallback to "default" variant. May still return `nullptr`
const sound_effect* find_random_effect( const std::string &id, const std::string& variant )
{
    const auto eff = find_random_effect( id_and_variant( id, variant ) );
    if( eff != nullptr ) {
        return eff;
    }
    return find_random_effect( id_and_variant( id, "default" ) );
}

// Contains the chunks that have been dynamically created via do_pitch_shift. It is used to
// distinguish between dynamically created chunks and static chunks (the later must not be freed).
std::set<Mix_Chunk*> dynamic_chunks;
// Deletes the dynamically created chunk (if such a chunk had been played).
void cleanup_when_channel_finished( int channel )
{
    Mix_Chunk *chunk = Mix_GetChunk( channel );
    const auto iter = dynamic_chunks.find( chunk );
    if( iter != dynamic_chunks.end() ) {
        dynamic_chunks.erase( iter );
        free( chunk->abuf );
        free( chunk );
    }
}

Mix_Chunk *do_pitch_shift( Mix_Chunk *s, float pitch ) {
    Mix_Chunk *result;
    Uint32 s_in = s->alen / 4;
    Uint32 s_out = ( Uint32 )( ( float )s_in * pitch );
    float pitch_real = ( float )s_out / ( float )s_in;
    Uint32 i, j;
    result = ( Mix_Chunk * )malloc( sizeof( Mix_Chunk ) );
    dynamic_chunks.insert( result );
    result->allocated = 1;
    result->alen = s_out * 4;
    result->abuf = ( Uint8* )malloc( result->alen * sizeof( Uint8 ) );
    result->volume = s->volume;
    for( i = 0; i < s_out; i++ ) {
        Sint16 lt;
        Sint16 rt;
        Sint16 lt_out;
        Sint16 rt_out;
        Sint64 lt_avg = 0;
        Sint64 rt_avg = 0;
        Uint32 begin = ( Uint32 )( ( float )i / pitch_real );
        Uint32 end = ( Uint32 )( ( float )( i + 1 ) / pitch_real );

        // check for boundary case
        if( end > 0 && ( end >= ( s->alen / 4 ) ) )
            end = begin;

        for( j = begin; j <= end; j++ ) {
            lt = ( s->abuf[( 4 * j ) + 1] << 8 ) | ( s->abuf[( 4 * j ) + 0] );
            rt = ( s->abuf[( 4 * j ) + 3] << 8 ) | ( s->abuf[( 4 * j ) + 2] );
            lt_avg += lt;
            rt_avg += rt;
        }
        lt_out = ( Sint16 )( ( float )lt_avg / ( float )( end - begin + 1 ) );
        rt_out = ( Sint16 )( ( float )rt_avg / ( float )( end - begin + 1 ) );
        result->abuf[( 4 * i ) + 1] = (Uint8)(( lt_out >> 8 ) & 0xFF);
        result->abuf[( 4 * i ) + 0] = (Uint8)(lt_out & 0xFF);
        result->abuf[( 4 * i ) + 3] = (Uint8)(( rt_out >> 8 ) & 0xFF);
        result->abuf[( 4 * i ) + 2] = (Uint8)(rt_out & 0xFF);
    }
    return result;
}

void sfx::play_variant_sound( std::string id, std::string variant, int volume ) {
    if( volume == 0 ) {
        return;
    }

    const sound_effect* eff = find_random_effect( id, variant );
    if( eff == nullptr ) {
        eff = find_random_effect( id, "default" );
        if( eff == nullptr ) {
            return;
        }
    }
    const sound_effect& selected_sound_effect = *eff;

    Mix_Chunk *effect_to_play = selected_sound_effect.chunk.get();
    Mix_VolumeChunk( effect_to_play,
                     selected_sound_effect.volume * get_option<int>( "SOUND_EFFECT_VOLUME" ) * volume / ( 100 * 100 ) );
    Mix_PlayChannel( -1, effect_to_play, 0 );
}

void sfx::play_variant_sound( std::string id, std::string variant, int volume, int angle,
                              float pitch_min, float pitch_max ) {
    if( volume == 0 ) {
        return;
    }

    const sound_effect* eff = find_random_effect( id, variant );
    if( eff == nullptr ) {
        return;
    }
    const sound_effect& selected_sound_effect = *eff;

    Mix_ChannelFinished( cleanup_when_channel_finished );
    Mix_Chunk *effect_to_play = selected_sound_effect.chunk.get();
    float pitch_random = rng_float( pitch_min, pitch_max );
    Mix_Chunk *shifted_effect = do_pitch_shift( effect_to_play, pitch_random );
    Mix_VolumeChunk( shifted_effect,
                     selected_sound_effect.volume * get_option<int>( "SOUND_EFFECT_VOLUME" ) * volume / ( 100 * 100 ) );
    int channel = Mix_PlayChannel( -1, shifted_effect, 0 );
    Mix_SetPosition( channel, angle, 1 );
}

void sfx::play_ambient_variant_sound( std::string id, std::string variant, int volume, int channel,
                                      int duration ) {
    if( volume == 0 ) {
        return;
    }

    const sound_effect* eff = find_random_effect( id, variant );
    if( eff == nullptr ) {
        return;
    }
    const sound_effect& selected_sound_effect = *eff;

    Mix_Chunk *effect_to_play = selected_sound_effect.chunk.get();
    Mix_VolumeChunk( effect_to_play,
                     selected_sound_effect.volume * get_option<int>( "SOUND_EFFECT_VOLUME" ) * volume / ( 100 * 100 ) );
    if( Mix_FadeInChannel( channel, effect_to_play, -1, duration ) == -1 ) {
        dbg( D_ERROR ) << "Failed to play sound effect: " << Mix_GetError();
    }
}
#endif

void load_soundset() {
#ifdef SDL_SOUND
    const std::string default_path = FILENAMES["defaultsounddir"];
    const std::string default_soundpack = "basic";
    std::string current_soundpack = get_option<std::string>( "SOUNDPACKS" );
    std::string soundpack_path;

    // Get curent soundpack and it's directory path.
    if (current_soundpack.empty()) {
        dbg( D_ERROR ) << "Soundpack not set in options or empty.";
        soundpack_path = default_path;
        current_soundpack = default_soundpack;
    } else {
        dbg( D_INFO ) << "Current soundpack is: " << current_soundpack;
        soundpack_path = SOUNDPACKS[current_soundpack];
    }

    if (soundpack_path.empty()) {
        dbg( D_ERROR ) << "Soundpack with name " << current_soundpack << " can't be found or empty string";
        soundpack_path = default_path;
        current_soundpack = default_soundpack;
    } else {
        dbg( D_INFO ) << '"' << current_soundpack << '"' << " soundpack: found path: " << soundpack_path;
    }

    current_soundpack_path = soundpack_path;
    try {
        DynamicDataLoader::get_instance().load_data_from_path( soundpack_path, "core" );
    } catch( const std::exception &err ) {
        dbg( D_ERROR ) << "failed to load sounds: " << err.what();
    }
#endif
}

void cleanup_sound() {
#ifdef SDL_SOUND
    sound_effects_p.clear();
    playlists.clear();
#endif
}

#endif // TILES
