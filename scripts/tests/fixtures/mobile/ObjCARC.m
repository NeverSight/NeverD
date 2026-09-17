// Owned fixture: compile with ARC; rebuild recovered methods without ARC.
#import <Foundation/Foundation.h>

@interface NDARCBox : NSObject
#ifdef NEVERD_ATOMIC_PROPERTIES
@property(atomic, strong) id item;
#else
@property(nonatomic, strong) id item;
#endif
@property(nonatomic, weak) id observer;
@property(nonatomic, copy) NSString *title;
@end

@implementation NDARCBox
@end
