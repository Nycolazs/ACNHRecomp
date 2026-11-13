// nn::web::ShowSharePage: the system "post to social media" page. Animal Crossing: New Horizons opens it through
// nn::album::SaveAndShareScreenshot after a design is posted to the Custom Designs Portal. The SDK launches it as a
// library applet; the applet creator here does not emulate it (ILibraryAppletCreator cmd 3), so the SDK read a
// null applet handle and crashed. The page is answered as closed by the player at once instead.
#include <cstdint>

#include <nx/runtime.h>

namespace nx {

namespace {

// shortcut: only the first u64 of SharePageReturnValue is written, assumed to be the exit reason as in the other
// nn::web return values (1 = closed with Back); write the rest once its layout is known.
void ShowSharePage(CPU& c) {
    if (c.x[0]) st<uint64_t>(c.x[0], 1);
    log("web: ShowSharePage answered as closed (the share page is not emulated)");
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn3web13ShowSharePageEPNS0_20SharePageReturnValueERKNS0_16ShowSharePageArgE", ShowSharePage);

}  // namespace nx
