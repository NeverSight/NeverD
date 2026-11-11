import fixture.*;
public class NestedVisibilitySuperBridgeCaller extends Outer {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void raw(Outer.Facade self, Context context, Bound input, long bits) {
        Hidden parent = self;
        parent.touch(context, input, bits);
    }
    public static Object call(Outer.Facade self, int operation, Context context, Outer input, long bits) {
        switch (operation) {
        case 0: return self.index();
        case 1: return self.flag();
        case 2: return self.wide();
        case 3: return self.ratio();
        case 4: return self.value();
        case 5: self.touch(context, input, bits); return null;
        case 6: return self.run(context, input, (int)bits);
        default: throw new AssertionError();
        }
    }
}
