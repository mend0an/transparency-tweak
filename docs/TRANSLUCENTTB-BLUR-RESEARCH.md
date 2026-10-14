# TranslucentTB taskbar blur research

Source snapshot: branch `release`, commit
`d4636e439865df0a1a1419db408e055740ce5c74`. A read-only checkout is kept locally
in `.tools/TranslucentTB-research/`.

## Important difference from the failed prototype

TranslucentTB's taskbar Acrylic uses `AcrylicBackgroundSource::Backdrop`.
Its source explains that a transparent taskbar window makes the underlying
desktop available behind the XAML, and that this choice avoids the inactive
window behavior of `HostBackdrop`.

Our failed prototype used `HostBackdrop`, an opaque fallback color, and brush
opacity 1. An inactive-window fallback is therefore a strong explanation for
the observed opaque background, but the exact runtime cause was not measured.
The experiment does not establish that all native taskbar Acrylic is unusable.

Source: [taskbar appearance service](https://github.com/TranslucentTB/TranslucentTB/blob/d4636e439865df0a1a1419db408e055740ce5c74/ExplorerTAP/taskbarappearanceservice.cpp).

## Adjustable glass blur

The separate `XamlBlurBrush` derives from `XamlCompositionBrushBase` and builds
an effect graph when connected:

```text
CreateBackdropBrush -> GaussianBlurEffect ---+
                                            +-> SourceOver composite -> taskbar fill
RGBA tint -----------> FloodEffect ----------+
```

Blur and tint alpha are independent. Keeping the brush opacity at 1 allows
zero tint alpha to retain the blurred background. Disconnecting closes the
Composition brush.

Source: [custom blur brush](https://github.com/TranslucentTB/TranslucentTB/blob/d4636e439865df0a1a1419db408e055740ce5c74/ExplorerTAP/XamlBlurBrush.cpp).

The XAML service obtains the compositor from the actual background element.
The worker passes `BlurRadius / 3`; the Gaussian adapter maps that amount to
Direct2D standard deviation. A UI percentage is consequently not a pixel radius.

Sources: [worker dispatch](https://github.com/TranslucentTB/TranslucentTB/blob/d4636e439865df0a1a1419db408e055740ce5c74/TranslucentTB/taskbar/taskbarattributeworker.cpp),
[Gaussian adapter](https://github.com/TranslucentTB/TranslucentTB/blob/d4636e439865df0a1a1419db408e055740ce5c74/ExplorerTAP/effects/GaussianBlurEffect.cpp).

## Modern versus legacy rendering

The worker selects a renderer from the detected taskbar type. XAML taskbars use
ExplorerTAP and its brush service; other taskbars use accent policy through
`SetWindowCompositionAttribute`. The menu explicitly enables Blur for XAML.
The README's Windows 10/build 22000 limitation does not describe the custom
XAML path present in this source snapshot.

Source: [menu support selection](https://github.com/TranslucentTB/TranslucentTB/blob/d4636e439865df0a1a1419db408e055740ce5c74/TranslucentTB/mainappwindow.cpp).

## Proposed experiments for this project

1. Test `Backdrop` Acrylic on the transparent taskbar window to isolate the
   source-selection issue. Check the appearance while other windows have focus.
2. For adjustable blur, implement a Composition effect brush using the
   taskbar element's compositor. Keep the existing transparent window backdrop
   path and separate the tint alpha from blur strength.
3. Constrain background matching to descendants of `TaskbarFrame`, and handle
   element removal and per-monitor brush lifetime.
4. Log actual effect creation and failures. Verify pixels with textured content
   behind the taskbar; successful setters do not prove visible blur.
5. Preserve the Settings scanner without diagnostics and rerun navigation,
   restoration, and transparent-mode checks.

This research changes no active renderer or installed binary. The working
baseline remains active; the prior stable backup and restore helper remain
available for a future experiment.
