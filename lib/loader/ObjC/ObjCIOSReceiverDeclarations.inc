// clang-format off
// Bounded public UIKit receiver facts, observed in both Xcode 26.5 arm64
// iPhoneOS and iPhoneSimulator Clang ASTs. These are receiver declarations,
// not globally unique selector signatures. The UIImage interface declares
// NSObject as superclass and adopts NSSecureCoding. Both ASTs agree on the
// method declarations below and their non-variadic canonical types. Enum
// UIImageOrientation has underlying type long; CGRect contains CGPoint and
// CGSize, each holding two doubles, in that order. Renderer format ownership
// and NSCopying adoption also agree in the device and simulator artifacts.
// Exact runtime encodings were checked by compiling those canonical method
// declarations with Apple Clang for arm64-apple-ios18.0; this does not claim a
// local compilation of the original UIKit headers. No x86_64 evidence exists.
// Source AST SHA-256:
// iPhoneOS:        79259f6825fd8d118d351d98a4530e426f55faaaa569b172ab14256a6a840ba7
// iPhoneSimulator: 707c4d1d4bf7f49570de9ad5f880a9398cd16b594468177307ba53332ba418b7
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", "C:NSObject|P:NSSecureCoding", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "images", "@16@0:8", "NSArray", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "CIImage", "@16@0:8", "CIImage", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "scale", "d16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "duration", "d16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "imageFlippedForRightToLeftLayoutDirection", "@16@0:8", "UIImage", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "imageOrientation", "q16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", true, "imageWithCGImage:", "@24@0:8^{CGImage=}16", "UIImage", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", true, "imageWithCGImage:scale:orientation:", "@40@0:8^{CGImage=}16d24q32", "UIImage", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "initWithCGImage:scale:orientation:", "@40@0:8^{CGImage=}16d24q32", "", true, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "drawInRect:", "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScreen", "", "C:NSObject", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScreen", "", true, "mainScreen", "@16@0:8", "UIScreen", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScreen", "", false, "scale", "d16@0:8", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsRendererFormat", "", "C:NSObject|P:NSCopying", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRendererFormat", "", "C:UIGraphicsRendererFormat", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsRendererFormat", "", true, "defaultFormat", "@16@0:8", "", true, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRendererFormat", "", false, "scale", "d16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRendererFormat", "", false, "setScale:", "v24@0:8d16", "", false, nullptr, "", false)
// UIProgressView receiver closure, observed in the complete Xcode 26.5 ASTs
// from GitHub Actions artifact 10653622473. Every superclass/protocol edge
// agrees; no parent declares setProgress:. Foundation and QuartzCore retain
// ownership of their existing parent declarations. Apple Clang independently
// encodes setProgress:(float) on both arm64 iOS targets as v20@0:8f16.
// iPhoneOS SHA-256: dc99c06a9e4aab8015f8675b08ac55e042201401c21d39a4df4535aa63e0303f
// Simulator SHA-256: c443f389aae208205f42b4a592859ffb45766cc6e2f663b6ef919528e2676b61
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIProgressView", "", "C:UIView|P:NSCoding", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIResponder", "", "C:NSObject|P:UIResponderStandardEditActions", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIView", "", "C:UIResponder|P:NSCoding|P:UIAppearance|P:UIAppearanceContainer|P:UIDynamicItem|P:UITraitEnvironment|P:UICoordinateSpace|P:UIFocusItem|P:UIFocusItemContainer|P:CALayerDelegate", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIAppearance", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIAppearanceContainer", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UICoordinateSpace", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIDynamicItem", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIFocusEnvironment", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIFocusItem", "", "P:UIFocusEnvironment", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIFocusItemContainer", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIResponderStandardEditActions", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UITraitEnvironment", "", "P:NSObject", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIProgressView", "", false, "setProgress:", "v20@0:8f16", "", false, nullptr, "", false)
// The same complete ASTs declare UIImageView with superclass UIView and no
// directly adopted protocols. This closes the inherited receiver scope for
// local subclasses without assigning a global type to their own selectors.
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImageView", "", "C:UIView", nullptr)
// The same complete ASTs above declare UIImage's bool HDR property and the
// UIActivityIndicatorView initializer, returning instancetype with a long enum.
// Preserve the activity indicator's exact UIView/NSCoding receiver closure.
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "isHighDynamicRange", "B16@0:8", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIActivityIndicatorView", "", "C:UIView|P:NSCoding", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIActivityIndicatorView", "", false, "initWithActivityIndicatorStyle:", "@24@0:8q16", "", true, nullptr, "", false)
// Navigation, scrolling and application declarations from the same complete
// device/simulator ASTs; retain their exact receiver hierarchy and results.
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIViewController", "", "C:UIResponder|P:NSCoding|P:UIAppearanceContainer|P:UITraitEnvironment|P:UIContentContainer|P:UIFocusEnvironment", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UINavigationController", "", "C:UIViewController", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScrollView", "", "C:UIView|P:NSCoding|P:UIFocusItemScrollableContainer", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGestureRecognizer", "", "C:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIContentContainer", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIApplication", "", "C:UIResponder", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIFocusItemScrollableContainer", "", "P:UIFocusItemContainer", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UINavigationController", "", false, "interactivePopGestureRecognizer", "@16@0:8", "UIGestureRecognizer", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIViewController", "", false, "parentViewController", "@16@0:8", "UIViewController", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScrollView", "", false, "flashScrollIndicators", "v16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIApplication", "", false, "endBackgroundTask:", "v24@0:8Q16", "", false, nullptr, "", false)
// clang-format on
