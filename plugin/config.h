// Plug-in configuration, read by the compiler, the resource scripts, CMake (version) and the licence audit.
// THE one definition of the product's name, IDs and version. A rename is the PLUG_NAME line.
#define PLUG_NAME "Magmora"
#define PLUG_MFR PLUG_NAME " Audio Project"
#define PLUG_VERSION_HEX 0x00000102
#define PLUG_VERSION_STR "0.1.2"
#define PLUG_UNIQUE_ID 'MgAp'
#define PLUG_MFR_ID 'Mgmr'
#define PLUG_URL_STR ""
#define PLUG_EMAIL_STR ""
// The manual: the README on the project's site, as a URL its section anchors can be appended to (for example
// "https://github.com/<owner>/<repo>/blob/main/README.md"). Empty: the editor shows no manual links.
#define MAGMORA_MANUAL_URL "https://github.com/vplsdev/magmora/blob/main/README.md"
#define PLUG_COPYRIGHT_STR "Copyright 2026 The Magmora contributors"
#define PLUG_CLASS_NAME Magmora

#define BUNDLE_NAME PLUG_NAME
#define BUNDLE_MFR "vpls"
#define BUNDLE_DOMAIN "dev"

#define SHARED_RESOURCES_SUBPATH PLUG_NAME

#ifdef APP_API
// The stand-alone takes the guitar from one input. iPlug2's host refuses a device with fewer inputs than the
// plug-in's maximum, which would rule out every one-input interface.
#define PLUG_CHANNEL_IO "1-2"
#else
#define PLUG_CHANNEL_IO "1-1 1-2 2-2"
#endif

#define PLUG_LATENCY 0
#define PLUG_TYPE 0
#define PLUG_DOES_MIDI_IN 0
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 1280
#define PLUG_HEIGHT 620
#define PLUG_MIN_WIDTH 960
#define PLUG_MIN_HEIGHT 465
#define PLUG_MAX_WIDTH 1920
#define PLUG_MAX_HEIGHT 930
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 1

#define AUV2_ENTRY Magmora_Entry
#define AUV2_ENTRY_STR "Magmora_Entry"
#define AUV2_FACTORY Magmora_Factory
#define AUV2_VIEW_CLASS Magmora_View
#define AUV2_VIEW_CLASS_STR "Magmora_View"

#define VST3_SUBCATEGORY "Fx|Distortion"

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
// iPlug2's host cuts the device buffer into vectors of this size and writes past the end of any buffer that is
// not a whole multiple of it (32, 441, ...). 1 is safe for every buffer; the engine's output does not depend on it.
#define APP_SIGNAL_VECTOR_SIZE 1
