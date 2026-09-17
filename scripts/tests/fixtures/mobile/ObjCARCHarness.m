#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>

@interface NDARCBox : NSObject
#ifdef NEVERD_ATOMIC_PROPERTIES
@property(atomic, strong) id item;
#else
@property(nonatomic, strong) id item;
#endif
@property(nonatomic, weak) id observer;
@property(nonatomic, copy) NSString *title;
@end

static int Destroyed;
@interface NDTracked : NSObject
@end
@implementation NDTracked
- (void)dealloc {
  ++Destroyed;
  [super dealloc];
}
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
  NSAutoreleasePool *Pool = [NSAutoreleasePool new];
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  NDARCBox *Box = [NDARCBox new];
  NDTracked *First = [NDTracked new];
  Box.item = First;
  [First release];
#ifdef NEVERD_ATOMIC_PROPERTIES
  NSAutoreleasePool *ReadPool = [NSAutoreleasePool new];
#endif
  if (Destroyed || Box.item != First)
    return 1;
#ifdef NEVERD_ATOMIC_PROPERTIES
  // The atomic getter owns an autoreleased retain. Clearing the property must
  // leave the object alive until that read's autorelease pool is drained.
  Box.item = nil;
  if (Destroyed)
    return 7;
  [ReadPool drain];
#endif
  Box.item = nil;
  if (Destroyed != 1)
    return 2;

  NDTracked *Second = [NDTracked new];
  Box.observer = Second;
  NSAutoreleasePool *WeakReadPool = [NSAutoreleasePool new];
  if (Box.observer != Second)
    return 6;
  [WeakReadPool drain];
  [Second release];
  if (Destroyed != 2 || Box.observer != nil)
    return 3;

  NSMutableString *Text = [NSMutableString stringWithString:@"original"];
  Box.title = Text;
  [Text appendString:@"-changed"];
  if (![Box.title isEqualToString:@"original"])
    return 4;

  NDTracked *Third = [NDTracked new];
  Box.item = Third;
  [Third release];
  [Box release];
  if (Destroyed != 3)
    return 5;
  [Pool drain];
  printf("strong=pass\nweak=pass\ncopy=pass\ndestructor=pass\ndestroyed=%d\n",
         Destroyed);
  return 0;
}
