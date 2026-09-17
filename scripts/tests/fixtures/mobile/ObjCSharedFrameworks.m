#import <CoreLocation/CoreLocation.h>
#import <CoreSpotlight/CoreSpotlight.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UserNotifications/UserNotifications.h>
@interface NDFrameworkCalls : NSObject
- (CALayer *)makeLayer;
- (NSString *)resizeGravity;
- (const void *)resizeGravityStorage;
- (float)opacity:(CALayer *)layer;
- (void)opacity:(float)value layer:(CALayer *)layer;
- (CGPoint)position:(CALayer *)layer;
- (void)position:(CGPoint)value layer:(CALayer *)layer;
- (CLLocationDistance)distance:(CLLocation *)first from:(CLLocation *)second;
- (void)describe:(CSSearchableItemAttributeSet *)attributes
            text:(NSString *)text;
- (NSString *)descriptionOf:(CSSearchableItemAttributeSet *)attributes;
- (UNNotificationRequest *)request:(NSString *)identifier
                           content:(UNNotificationContent *)content;
- (UTType *)type:(NSString *)identifier;
- (NSString *)extensionOf:(UTType *)type;
@end
@implementation NDFrameworkCalls
- (NSString *)resizeGravity {
  return kCAGravityResize;
}
- (const void *)resizeGravityStorage {
  return &kCAGravityResize;
}
- (CALayer *)makeLayer {
  return [CALayer layer];
}
- (float)opacity:(CALayer *)layer {
  return layer.opacity;
}
- (void)opacity:(float)value layer:(CALayer *)layer {
  layer.opacity = value;
}
- (CGPoint)position:(CALayer *)layer {
  return layer.position;
}
- (void)position:(CGPoint)value layer:(CALayer *)layer {
  layer.position = value;
}
- (CLLocationDistance)distance:(CLLocation *)first from:(CLLocation *)second {
  return [first distanceFromLocation:second];
}
- (void)describe:(CSSearchableItemAttributeSet *)attributes
            text:(NSString *)text {
  attributes.contentDescription = text;
}
- (NSString *)descriptionOf:(CSSearchableItemAttributeSet *)attributes {
  return attributes.contentDescription;
}
- (UNNotificationRequest *)request:(NSString *)identifier
                           content:(UNNotificationContent *)content {
  return [UNNotificationRequest requestWithIdentifier:identifier
                                              content:content
                                              trigger:nil];
}
- (UTType *)type:(NSString *)identifier {
  return [UTType typeWithIdentifier:identifier];
}
- (NSString *)extensionOf:(UTType *)type {
  return type.preferredFilenameExtension;
}
@end
