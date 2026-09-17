#import <CoreLocation/CoreLocation.h>
#import <CoreSpotlight/CoreSpotlight.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UserNotifications/UserNotifications.h>
#import <objc/runtime.h>
#include <stdio.h>
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
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDFrameworkCalls *calls = [NDFrameworkCalls new];
    CLLocation *first = [[CLLocation alloc] initWithLatitude:1.0 longitude:2.0];
    CLLocation *second = [[CLLocation alloc] initWithLatitude:3.0
                                                    longitude:4.0];
    CSSearchableItemAttributeSet *attributes =
        [[CSSearchableItemAttributeSet alloc] initWithContentType:UTTypeText];
    UNMutableNotificationContent *content = [UNMutableNotificationContent new];
    content.body = @"contents";
    for (unsigned i = 0; i < 256; ++i) {
      @autoreleasepool {
        if ([calls resizeGravity] != kCAGravityResize ||
            [calls resizeGravityStorage] != &kCAGravityResize)
          return 8;
        CALayer *layer = [calls makeLayer];
        if (![layer isKindOfClass:CALayer.class] || layer.opacity != 1.0f)
          return 1;
        float opacity = (i % 128) / 128.0f;
        [calls opacity:opacity layer:layer];
        if (layer.opacity != opacity || [calls opacity:layer] != opacity)
          return 2;
        CGPoint position = CGPointMake((double)i * 0.125, -(double)i * 0.25);
        [calls position:position layer:layer];
        CGPoint result = [calls position:layer];
        if (result.x != position.x || result.y != position.y ||
            layer.position.x != position.x || layer.position.y != position.y)
          return 3;
        if ([calls distance:first
                       from:second] != [first distanceFromLocation:second])
          return 4;
        NSString *text = [NSString stringWithFormat:@"description-%u", i];
        [calls describe:attributes text:text];
        if (![attributes.contentDescription isEqual:text] ||
            ![[calls descriptionOf:attributes] isEqual:text])
          return 5;
        UNNotificationRequest *request = [calls request:text content:content];
        if (![request.identifier isEqual:text] ||
            ![request.content.body isEqual:content.body] ||
            request.trigger != nil)
          return 6;
        UTType *type = [calls type:@"public.png"];
        if (![type isEqual:UTTypePNG] ||
            ![[calls extensionOf:type] isEqual:@"png"])
          return 7;
      }
    }
    [content release];
    [attributes release];
    [second release];
    [first release];
    [calls release];
    puts("framework-calls=3328\nscalar-record-values=pass\nobject-identity="
         "pass");
  }
  return 0;
}
