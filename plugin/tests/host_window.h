// The editor in a window that is never put on screen, with synthetic input and a way to read its pixels back.
// Only the plug-in's own Metal drawable is read: nothing here can see the screen or another window.
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>

#include "host_plugs.h"

static CGFloat gScaleOverride = 0.;  // 0: the real one
static NSPoint gMouse = {0., 0.};
static id<MTLTexture> gFrame = nil;
static int gFrames = 0;

@interface QuietWindow : NSWindow
@end
@implementation QuietWindow
- (CGFloat)backingScaleFactor { return gScaleOverride > 0. ? gScaleOverride : [super backingScaleFactor]; }
- (void)makeKeyAndOrderFront:(id)sender { }
- (void)orderFront:(id)sender { }
- (BOOL)canBecomeKeyWindow { return YES; }
- (NSRect)constrainFrameRect:(NSRect)frame toScreen:(NSScreen*)screen { return frame; }
@end

// What the plug-in's view reads of an event.
@interface FakeEvent : NSObject
@property(nonatomic) NSPoint locationInWindow;
@property(nonatomic) NSUInteger modifierFlags;
@property(nonatomic) NSInteger clickCount;
@property(nonatomic) CGFloat deltaY;
@property(nonatomic) NSEventSubtype subtype;
@end
@implementation FakeEvent
@end

namespace host {

inline void InstallHooks() {
  static bool done = false;
  if (done) return;
  done = true;
  // Remember each frame the plug-in is about to present.
  Method next = class_getInstanceMethod(CAMetalLayer.class, @selector(nextDrawable));
  using Next = id (*)(id, SEL);
  Next original = reinterpret_cast<Next>(method_getImplementation(next));
  method_setImplementation(next, imp_implementationWithBlock(^id(CAMetalLayer* layer) {
    if (layer.framebufferOnly) layer.framebufferOnly = NO;
    id<CAMetalDrawable> drawable = original(layer, @selector(nextDrawable));
    if (drawable) {
      gFrame = drawable.texture;
      ++gFrames;
    }
    return drawable;
  }));
  // The knobs lock the pointer to where AppKit says it is: that has to be the synthetic pointer.
  Method where = class_getClassMethod(NSEvent.class, @selector(mouseLocation));
  method_setImplementation(where, imp_implementationWithBlock(^NSPoint(id) { return gMouse; }));
}

inline void Pump(double seconds) {
  const NSTimeInterval until = NSDate.timeIntervalSinceReferenceDate + seconds;
  do {
    @autoreleasepool {
      [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.004]];
    }
  } while (NSDate.timeIntervalSinceReferenceDate < until);
}

enum Keys : NSUInteger {
  kNone = 0,
  kShift = NSEventModifierFlagShift,
  kOption = NSEventModifierFlagOption,
  kCommand = NSEventModifierFlagCommand,
};

class Window {
public:
  Window(Plug& plug, double screenScale) : mPlug(plug) {
    InstallHooks();
    gScaleOverride = screenScale;
    gFrame = nil;
    mWindow = [[QuietWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 1280, 620)
                                             styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    mWindow.releasedWhenClosed = NO;
    plug.resized = [this](int w, int h) { [mWindow setContentSize:NSMakeSize(w, h)]; };
    mView = plug.OpenView(mWindow.contentView);
    if (!mView) Die("no editor");
    Pump(0.1);
    Fit();
    if (screenScale > 0.) {
      // With a pretended screen scale AppKit cannot size the drawable for us.
      static_cast<CAMetalLayer*>(mView.layer).drawableSize =
          CGSizeMake(mView.frame.size.width * screenScale, mView.frame.size.height * screenScale);
    }
    Pump(0.3);
  }

  ~Window() {
    mPlug.resized = nullptr;
    mPlug.CloseView();
    mView = nil;
    [mWindow close];
    Pump(0.05);
  }

  NSView* View() const { return mView; }
  double Scale() const { return mView.frame.size.width / 1280.; }

  // Follow the view, as a host does.
  void Fit() {
    [mWindow setContentSize:mView.frame.size];
    [mView setFrameOrigin:NSZeroPoint];
  }

  // Canvas points, origin top left, as in layout.json.
  void Move(double x, double y, NSUInteger keys = kNone) {
    [mView mouseMoved:Event(x, y, keys, 0)];
    Pump(0.03);
  }

  void Down(double x, double y, NSUInteger keys = kNone, int clicks = 1) {
    EndEditing();
    [mView mouseMoved:Event(x, y, keys, 0)];
    [mView mouseDown:Event(x, y, keys, clicks)];
  }

  void Up(double x, double y, NSUInteger keys = kNone, int clicks = 1) {
    [mView mouseUp:Event(x, y, keys, clicks)];
    Pump(0.05);
    Fit();
  }

  void Click(double x, double y, NSUInteger keys = kNone, int clicks = 1) {
    Down(x, y, keys, clicks);
    Up(x, y, keys, clicks);
  }

  void DoubleClick(double x, double y) {
    Click(x, y, kNone, 1);
    Click(x, y, kNone, 2);
  }

  void RightClick(double x, double y) {
    EndEditing();
    [mView rightMouseDown:Event(x, y, kNone, 1)];
    [mView rightMouseUp:Event(x, y, kNone, 1)];
    Pump(0.05);
  }

  // A drag of `points` screen points upwards, the pointer locked after the first movement as on a real screen.
  void DragUp(double x, double y, double points, NSUInteger keys = kNone, int steps = 10, bool release = true) {
    Down(x, y, keys);
    HoldDrag(x, y, points, keys, steps);
    if (release) Up(x, y, keys);
  }

  void HoldDrag(double x, double y, double points, NSUInteger keys, int steps) {
    const double d = points / steps / Scale();
    [mView mouseDragged:Event(x, y - d, keys, 1)];
    // The plug-in locks the pointer to whole screen points, and the hardware moves it from there.
    const NSPoint lock = NSMakePoint(std::round(gMouse.x), std::round(gMouse.y));
    const NSPoint locked = [mWindow convertRectFromScreen:NSMakeRect(lock.x, lock.y, 0, 0)].origin;
    const NSPoint inView = [mView convertPoint:locked fromView:nil];
    for (int i = 1; i < steps; ++i) {
      FakeEvent* e = (FakeEvent*)Event(inView.x / Scale(), inView.y / Scale() - d, keys, 1);
      gMouse = lock;
      [mView mouseDragged:(NSEvent*)e];
      Pump(0.004);
    }
  }

  void Wheel(double x, double y, double delta, NSUInteger keys = kNone) {
    FakeEvent* e = (FakeEvent*)Event(x, y, keys, 0);
    e.deltaY = delta;
    [mView mouseMoved:(NSEvent*)e];
    [mView scrollWheel:(NSEvent*)e];
  }

  NSTextField* Field() const {
    for (NSView* v in mView.subviews)
      if ([v isKindOfClass:NSTextField.class]) return (NSTextField*)v;
    return nil;
  }

  bool Type(const char* text) {
    NSTextField* field = Field();
    if (!field) return false;
    NSText* editor = field.currentEditor;
    if (editor)
      editor.string = @(text);
    else
      field.stringValue = @(text);
    [mWindow makeFirstResponder:mView];
    Pump(0.05);
    return true;
  }

  // A click elsewhere ends a text field's editing before the click arrives, as AppKit does when the view under the
  // pointer takes the keyboard.
  void EndEditing() {
    if (Field()) [mWindow makeFirstResponder:mView];
  }

  // The frame the plug-in last presented, as BGRA, after `wait` seconds of drawing.
  bool Capture(std::vector<uint8_t>& pixels, int& w, int& h, double wait = 0.25) {
    Pump(wait);
    if (!gFrame) return false;
    id<MTLTexture> t = gFrame;
    w = static_cast<int>(t.width);
    h = static_cast<int>(t.height);
    const NSUInteger row = (w * 4 + 255) & ~NSUInteger(255);
    id<MTLCommandQueue> queue = [t.device newCommandQueue];
    id<MTLBuffer> buffer = [t.device newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:buffer destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    pixels.resize(w * h * 4);
    for (int y = 0; y < h; ++y)
      std::memcpy(&pixels[y * w * 4], static_cast<const uint8_t*>(buffer.contents) + row * y, w * 4);
    return true;
  }

  bool SavePng(const std::string& path) {
    std::vector<uint8_t> bgra;
    int w = 0, h = 0;
    if (!Capture(bgra, w, h)) return false;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:w pixelsHigh:h
                                                                 bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO
                                                                      isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:w * 3 bitsPerPixel:24];
    uint8_t* to = rep.bitmapData;
    for (int i = 0; i < w * h; ++i) {
      to[3 * i] = bgra[4 * i + 2];
      to[3 * i + 1] = bgra[4 * i + 1];
      to[3 * i + 2] = bgra[4 * i];
    }
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:@(path.c_str()) atomically:YES];
  }

private:
  NSEvent* Event(double x, double y, NSUInteger keys, int clicks) {
    const double s = Scale();
    const NSPoint inView = NSMakePoint(x * s, y * s);
    const NSPoint inWindow = [mView convertPoint:inView toView:nil];
    gMouse = [mWindow convertRectToScreen:NSMakeRect(inWindow.x, inWindow.y, 0, 0)].origin;
    FakeEvent* e = [[FakeEvent alloc] init];
    e.locationInWindow = inWindow;
    e.modifierFlags = keys;
    e.clickCount = clicks;
    return (NSEvent*)e;
  }

  Plug& mPlug;
  QuietWindow* mWindow;
  NSView* mView;
};

}  // namespace host
