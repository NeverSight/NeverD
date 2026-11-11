import fixture.Box;
import fixture.Mapper;
import fixture.Receiver;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

public final class CopiedInstanceDefaultScopeHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Instance default scope behavior changed");
    }
    public static void main(String[] args) throws Exception {
        Receiver.slots = new Object[17];
        for (int i = 0; i < Receiver.slots.length; ++i) Receiver.slots[i] = new Object();
        int probes = 0;
        Mapper generated = null;
        for (Object token : new Object[] {new Object(), "owned", Integer.valueOf(37), null}) {
            Receiver<Object> receiver = new Receiver<Object>(token);
            Mapper supplied = new Mapper() {
                public Object apply(Box<Object> box) { return token; }
            };
            require(receiver.pick(supplied) == supplied && receiver.reads == 1);
            require(Receiver.pick$default(receiver, supplied, 0, null) == supplied && receiver.reads == 2);
            require(Receiver.pick$default(receiver, null, 0, null) == null && receiver.reads == 3);
            generated = Receiver.pick$default(receiver, supplied, 1, null);
            require(generated != supplied && receiver.reads == 4);
            Box<Object> box = new Box<Object>(token);
            require(generated.apply(box) == token);
            Object next = new Object();
            box.value = next;
            require(generated.apply(box) == next);
            probes += 2;
        }
        try { generated.apply(null); throw new AssertionError("missing null box failure"); }
        catch (NullPointerException expected) {}
        Receiver<Object> receiver = new Receiver<Object>(null);
        receiver.failure = new RuntimeException("owned sentinel");
        try { Receiver.pick$default(receiver, null, 1, null); throw new AssertionError("missing receiver failure"); }
        catch (RuntimeException actual) { require(actual == receiver.failure && receiver.reads == 1); }
        try { Receiver.pick$default(null, null, 1, null); throw new AssertionError("missing null receiver failure"); }
        catch (NullPointerException expected) {}
        require(Receiver.sinkCalls == 16);
        Method apply = generated.getClass().getDeclaredMethod("apply", Box.class);
        Type argument = ((ParameterizedType) apply.getGenericParameterTypes()[0]).getActualTypeArguments()[0];
        if (args[0].equals("original")) {
            require(argument instanceof TypeVariable && ((TypeVariable<?>) argument).getName().equals("E")
                    && ((TypeVariable<?>) argument).getGenericDeclaration() == Receiver.class);
        } else require(argument == Object.class);
        System.out.println("identity-probes=" + probes + ";receiver-calls=17;receiver-failures=1;null-failures=2");
    }
}
