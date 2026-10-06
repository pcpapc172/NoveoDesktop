"""Apply the call-only media opt-in to a build-directory copy of pinned lib_webview.

Other embedded web content, including the restricted proxy bridge, never opts in.
Fail closed on unexpected upstream changes rather than silently dropping a patch.
"""
import shutil
import sys
import tempfile
from pathlib import Path
source, destination = map(Path, sys.argv[1:])
staging = tempfile.TemporaryDirectory(prefix='noveo-webview-')
output = Path(staging.name) / 'sources'
shutil.copytree(source, output, ignore=shutil.ignore_patterns('.git'))

def edit(name, old, new):
    path = output / name
    text = path.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f'{name}: expected one patch anchor, found {text.count(old)}')
    path.write_text(text.replace(old, new))

for name in ('webview/webview_embed.h', 'webview/webview_interface.h'):
    edit(name, '\tbool allowThirdPartyCookies = false;', '\tbool allowThirdPartyCookies = false;\n\tbool allowCallMedia = false;')
edit('webview/webview_embed.cpp', '\t\t.allowThirdPartyCookies = config.allowThirdPartyCookies,', '\t\t.allowThirdPartyCookies = config.allowThirdPartyCookies,\n\t\t.allowCallMedia = config.allowCallMedia,')
linux = 'webview/platform/linux/webview_linux_webkitgtk.cpp'
edit(linux, '\tstd::string _shellMessageToken;', '\tstd::string _shellMessageToken;\n\tbool _allowCallMedia = false;')
edit(linux, '\t_shellMessageToken = std::move(config.shellMessageToken);', '\t_shellMessageToken = std::move(config.shellMessageToken);\n\t_allowCallMedia = config.allowCallMedia && _restrictedOrigin.empty();')
edit(linux, '\t\t\tallowThirdPartyCookies,\n', '\t\t\tallowThirdPartyCookies,\n\t\t\t_allowCallMedia,\n')
edit(linux, '\t\t\tbool allowThirdPartyCookies,\n', '\t\t\tbool allowThirdPartyCookies,\n\t\t\tbool allowCallMedia,\n')
edit(linux, '\t\t\t.allowThirdPartyCookies = allowThirdPartyCookies,', '\t\t\t.allowThirdPartyCookies = allowThirdPartyCookies,\n\t\t\t.allowCallMedia = allowCallMedia,')
edit('webview/platform/linux/webview_linux_interface.xml', "\t\t\t<arg type='b' name='allowThirdPartyCookies' direction='in'/>", "\t\t\t<arg type='b' name='allowThirdPartyCookies' direction='in'/>\n\t\t\t<arg type='b' name='allowCallMedia' direction='in'/>")
edit(linux, '\tWebKitUserContentManager *manager =\n', '''	if (_allowCallMedia) {
		const auto settings = webkit_web_view_get_settings(_webview);
		if (webkit_settings_set_enable_webrtc) webkit_settings_set_enable_webrtc(settings, true);
		if (webkit_settings_set_enable_media_stream) webkit_settings_set_enable_media_stream(settings, true);
		if (webkit_settings_set_enable_media) webkit_settings_set_enable_media(settings, true);
		if (webkit_settings_set_media_playback_requires_user_gesture) {
			webkit_settings_set_media_playback_requires_user_gesture(settings, false);
		}
	}
	WebKitUserContentManager *manager =
''')
edit(linux, '\t// navigator.clipboard.read/readText() asks for this one.', '''	if (_allowCallMedia && webkit_user_media_permission_request_get_type
		&& webkit_permission_request_allow
		&& G_TYPE_CHECK_INSTANCE_TYPE(request, webkit_user_media_permission_request_get_type())) {
		webkit_permission_request_allow(request);
		return true;
	}
	// navigator.clipboard.read/readText() asks for this one.''')
# The bridge grants media only to bundled content with navigation blocked natively.
# No notification, bot, game or proxy webview sets allowCallMedia.
edit('webview/platform/linux/webview_linux_webkitgtk_library.h', 'inline void (*webkit_permission_request_deny)(', '''inline GType (*webkit_user_media_permission_request_get_type)(void);
inline void (*webkit_permission_request_allow)(WebKitPermissionRequest *request);
inline void (*webkit_permission_request_deny)(''')
edit('webview/platform/linux/webview_linux_webkitgtk_library.cpp', '\tLOAD_LIBRARY_SYMBOL(lib, webkit_permission_request_deny);', '''	LOAD_LIBRARY_SYMBOL(lib, webkit_permission_request_deny);
	LOAD_LIBRARY_SYMBOL(lib, webkit_permission_request_allow);
	LOAD_LIBRARY_SYMBOL(lib, webkit_user_media_permission_request_get_type);''')
win = 'webview/platform/win/webview_windows_edge_chromium.cpp'
edit(win, '\tstd::string _restrictedOrigin;', '\tstd::string _restrictedOrigin;\n\tbool _allowCallMedia = false;')
edit(win, ', _debug(config.debug) {', ', _allowCallMedia(config.allowCallMedia && _restrictedOrigin.empty())\n, _debug(config.debug) {')
edit(win, '\t\t} else if (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ) {', '''		} else if (_allowCallMedia && (kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE
			|| kind == COREWEBVIEW2_PERMISSION_KIND_CAMERA)) {
			args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
		} else if (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ) {''')
edit(win, 'options->put_AdditionalBrowserArguments(config.restrictedOrigin.empty()', '''options->put_AdditionalBrowserArguments(config.allowCallMedia && config.restrictedOrigin.empty()
		? L"--autoplay-policy=no-user-gesture-required "
			L"--disable-features=ElasticOverscroll,AutofillAiWalletPrivatePasses"
		: config.restrictedOrigin.empty()''')

# The call document has Noveo's HTTPS origin, matching Android's base URL and
# the server's Origin checks. Only its narrow resource path is intercepted.
edit(win, '\tstd::string _restrictedOrigin;\n\tbool _allowCallMedia = false;',
    '\tstd::string _restrictedOrigin;\n\tstd::string_view _dataUrlPrefix = kDataUrlPrefix;\n\tbool _allowCallMedia = false;')
edit(win, ', _allowCallMedia(config.allowCallMedia && _restrictedOrigin.empty())',
    ', _dataUrlPrefix(config.allowCallMedia ? std::string_view("https://noveo.ir/noveo-desktop-call/") : kDataUrlPrefix)\n, _allowCallMedia(config.allowCallMedia && _restrictedOrigin.empty())')
edit(win, '(ToWide(kDataUrlPrefix) + L\'*\')', '(ToWide(_dataUrlPrefix) + L\'*\')')
edit(win, 'const auto prefix = kDataUrlPrefix.size();', 'const auto prefix = _dataUrlPrefix.size();')
edit(win, 'ansi.compare(0, prefix, kDataUrlPrefix)', 'ansi.compare(0, prefix, _dataUrlPrefix)')
edit(win, '\tbase::Timer _keepActiveTimer;', '\tstd::string_view _dataUrlPrefix = kDataUrlPrefix;\n\tbase::Timer _keepActiveTimer;')
edit(win, 'void Instance::start(Config &&config) {', 'void Instance::start(Config &&config) {\n\tif (config.allowCallMedia) _dataUrlPrefix = "https://noveo.ir/noveo-desktop-call/";')
edit(win, 'full.reserve(kDataUrlPrefix.size()', 'full.reserve(_dataUrlPrefix.size()')
edit(win, 'full.append(kDataUrlPrefix);', 'full.append(_dataUrlPrefix);')

# Preserve timestamps for identical generated inputs and incremental builds.
for path in output.rglob('*'):
    if path.is_file():
        target = destination / path.relative_to(output)
        if not target.exists() or target.read_bytes() != path.read_bytes():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
staging.cleanup()
