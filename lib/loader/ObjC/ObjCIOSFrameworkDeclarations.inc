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
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGImage", "^{CGImage=}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGColor", "^{CGColor=}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CIImage", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGSizeValue", "{CGSize=dd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "alpha", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "clearColor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "colorWithRed:green:blue:alpha:", "@48@0:8d16d24d32d40", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "currentDevice", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "currentEDRHeadroom", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "dismissViewControllerAnimated:completion:", "v28@0:8B16@?20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "displayScale", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "getRed:green:blue:alpha:", "B48@0:8^d16^d24^d32^d40", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "getWhite:alpha:", "B32@0:8^d16^d24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageFlippedForRightToLeftLayoutDirection", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageOrientation", "q16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageWithCGImage:", "@24@0:8^{CGImage=}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageWithCIImage:scale:orientation:", "@40@0:8@16d24q32", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "mainScreen", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "potentialEDRHeadroom", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "scale", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setActivityIndicatorViewStyle:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "superview", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "traitCollection", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "userInterfaceIdiom", "q16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "window", "@16@0:8", nullptr},
    // clang-format on
