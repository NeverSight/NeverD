// clang-format off
// Bounded public MapKit declaration: MKPlacemark.h in MacOSX SDK 15.5
// declares this exact method, and Apple Clang emits the encoding below for
// arm64 macOS. The SDK's arm64 Mac Catalyst declaration agrees, and the public
// iOS API has the same two-double coordinate and object argument. Apple Clang
// confirms the same encoding and HFA carrier on an arm64 iOS simulator target.
// No x86_64 ABI is inferred. Exact system MapKit linkage is required.
{"MapKit", "/System/Library/Frameworks/MapKit.framework/MapKit|/System/Library/Frameworks/MapKit.framework/Versions/A/MapKit", "initWithCoordinate:addressDictionary:", "@40@0:8{CLLocationCoordinate2D=dd}16@32", nullptr},
// clang-format on
