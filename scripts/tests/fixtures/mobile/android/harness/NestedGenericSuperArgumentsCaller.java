import fixture.*;
public class NestedGenericSuperArgumentsCaller {
    public static Object call(Caller<Bound> self, Context context, Bound input, long bits) {
        return self.read(context,input,bits);
    }
}
