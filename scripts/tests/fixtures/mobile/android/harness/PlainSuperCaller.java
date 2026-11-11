import fixture.*;
public final class PlainSuperCaller {
    public static void run(Owner.Child child, Box<Object> box, Object token, int code) { child.run(box, token, code); }
}
