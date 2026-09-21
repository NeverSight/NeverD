// clang-format off
// Compiler-observed public UIKit API facts for arm64 iOS.
// Xcode 26.5 iPhoneOS and arm64 iPhoneSimulator ASTs agree on each owned
// declaration and its desugared types. Exact arm64 Objective-C encodings were
// independently produced from the public UIKit headers by Apple Clang. This
// catalog includes only declarations observed in both artifacts; selectors
// whose owners disagree remain absent or are resolved by receiver evidence.
// The source AST SHA-256 values are:
//   iPhoneOS:       79259f6825fd8d118d351d98a4530e426f55faaaa569b172ab14256a6a840ba7
//   iPhoneSimulator:707c4d1d4bf7f49570de9ad5f880a9398cd16b594468177307ba53332ba418b7
// No x86_64 iPhoneSimulator declaration artifact was available, so that
// architecture remains unsupported instead of borrowing Mac Catalyst facts.
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "contextWithOptions:", "@24@0:8@16", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "createCGImage:fromRect:", "^{CGImage=}56@0:8@16{CGRect={CGPoint=dd}{CGSize=dd}}24", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "detectorOfType:context:options:", "@40@0:8@16@24@32", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "extent", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "filterWithName:", "@24@0:8@16", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "imageByCroppingToRect:", "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "outputImage", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGColor", "^{CGColor=}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGImage", "^{CGImage=}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGSizeValue", "{CGSize=dd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CIImage", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "addSubview:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "alpha", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "blackColor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "clearColor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "colorWithAlphaComponent:", "@24@0:8d16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "colorWithRed:green:blue:alpha:", "@48@0:8d16d24d32d40", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "currentDevice", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "currentEDRHeadroom", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "dismissViewControllerAnimated:completion:", "v28@0:8B16@?20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "displayScale", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "getRed:green:blue:alpha:", "B48@0:8^d16^d24^d32^d40", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "getWhite:alpha:", "B32@0:8^d16^d24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageFlippedForRightToLeftLayoutDirection", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageForState:", "@24@0:8Q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageOrientation", "q16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageWithCGImage:", "@24@0:8^{CGImage=}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageWithCIImage:scale:orientation:", "@40@0:8@16d24q32", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithCGImage:", "@24@0:8^{CGImage=}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithProgressViewStyle:", "@24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithRed:green:blue:alpha:", "@48@0:8d16d24d32d40", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "insertSubview:belowSubview:", "v32@0:8@16@24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "instantiateWithOwner:options:", "@32@0:8@16@24", nullptr},
// The complete device/simulator ASTs cited for UIProgressView below also agree
// on UIView(UIConstraintBasedLayoutLayering)'s void invalidation method and
// UIButton's UIColor* plus UIControlState (underlying unsigned long) setter.
// These are the only owners of either selector in both complete artifacts.
// Apple Clang independently reproduces both encodings from those canonical
// declaration types for arm64 iOS and arm64 iOS Simulator.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "invalidateIntrinsicContentSize", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "mainScreen", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "nibWithNibName:bundle:", "@32@0:8@16@24", nullptr},
// UIProgressView additions: both complete Xcode 26.5 Messages ASTs from
// GitHub Actions artifact 10653622473 agree on every owner of these selectors.
// iPhoneOS SHA-256: dc99c06a9e4aab8015f8675b08ac55e042201401c21d39a4df4535aa63e0303f
// Simulator SHA-256: c443f389aae208205f42b4a592859ffb45766cc6e2f663b6ef919528e2676b61
// Apple Clang independently encodes the observed NSProgress*, float and bool
// declaration types for both arm64 iOS targets; neither is variadic.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "observedProgress", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "potentialEDRHeadroom", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "resignFirstResponder", "B16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "scale", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "sendActionsForControlEvents:", "v24@0:8Q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setAccessibilityIgnoresInvertColors:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setActivityIndicatorViewStyle:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setAdjustsFontForContentSizeCategory:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setCenter:", "v32@0:8{CGPoint=dd}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setLayoutMargins:", "v48@0:8{UIEdgeInsets=dddd}16", nullptr},
// Both ASTs cited above also declare setProgress: with incompatible owners:
// UIProgressView takes float, while UIPageControl takes an object pointer.
// Keep both alternatives; only receiver evidence may select either one.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setProgress:", "v20@0:8f16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setProgress:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setProgress:animated:", "v24@0:8f16B20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setProgressViewStyle:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setText:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setTitleColor:forState:", "v32@0:8@16Q24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setTranslatesAutoresizingMaskIntoConstraints:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "sizeToFit", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "superview", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "systemVersion", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "topViewController", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "traitCollection", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "userInterfaceIdiom", "q16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "window", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "whiteColor", "@16@0:8", nullptr},
    // clang-format on
