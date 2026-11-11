import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PlainSuperHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("plain superclass contract"); }
    static void call(Owner.Child receiver, Box<Object> box, Object token, int code, int route) throws Throwable {
        if (route == 0) receiver.run(box, token, code);
        else if (route == 1) PlainSuperCaller.run(receiver, box, token, code);
        else if (route == 2) {
            try { Owner.Child.class.getMethod("run", Box.class, Object.class, int.class).invoke(receiver, box, token, code); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else MethodHandles.publicLookup().findVirtual(Owner.Child.class, "run",
            MethodType.methodType(void.class, Box.class, Object.class, int.class)).invokeExact(receiver, box, token, code);
    }
    public static void main(String[] args) throws Throwable {
        int branch = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Owner owner = new Owner(); Owner.Child receiver = owner.new Child();
        Box<Object> box = mode == 5 ? null : new Box<>();
        int[] input = {11}, replacement = {17};
        Object token = mode == 6 ? null : input;
        Trace.failure = new IllegalStateException("owned superclass failure");
        Trace.failAt = mode >= 1 && mode <= 4 ? mode : 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            Object sentinel = new Object(); Trace.receiver = Trace.box = Trace.token = sentinel; Trace.code = 19;
            Trace.events = 0; replacement[0]++; input[0]++;
            owner.replacement = branch == 0 ? null : replacement;
            if (box != null) box.value = sentinel;
            Object selected = branch == 0 ? token : replacement;
            int code = attempt == 0 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
            Throwable failure = null;
            try { call(receiver, box, token, code, route); } catch (Throwable caught) { failure = caught; }
            int expected = mode == 1 ? 1 : mode == 2 || mode == 5 ? 12 : mode == 3 ? 123 : 1234;
            require(Trace.events == expected);
            if (mode >= 1 && mode <= 4) require(failure == Trace.failure);
            else if (mode == 5) require(failure instanceof NullPointerException);
            else require(failure == null);
            boolean reached = mode != 1 && mode != 2;
            require(Trace.receiver == (reached ? receiver : sentinel));
            require(Trace.box == (reached ? box : sentinel) && Trace.token == (reached ? selected : sentinel));
            require(Trace.code == (reached ? code : 19));
            if (box != null) require(box.value == (reached ? selected : sentinel));
            if (reached && box != null && selected != null) require(((int[]) box.value)[0] == (branch == 0 ? input[0] : replacement[0]));
            System.out.println("branch=" + branch + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
