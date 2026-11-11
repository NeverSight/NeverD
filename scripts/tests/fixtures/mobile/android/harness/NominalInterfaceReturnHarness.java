import fixture.*;
import java.lang.reflect.*;

public final class NominalInterfaceReturnHarness {
    private static int identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void same(Object a, Object b) { check(a == b); identities++; }
    private static final class Value implements Fixed {
        Tag selected;
        public Tag read() { return selected; }
        public void write(Tag tag) { selected = tag; }
    }
    private static class Producer implements Refined {
        Fixed selected;
        RuntimeException failure;
        int calls;
        public Fixed create() { calls++; if (failure != null) throw failure; return selected; }
    }
    private static final class Child extends Producer {
        public Fixed create() { calls += 2; return selected; }
    }
    public static void main(String[] args) throws Exception {
        Producer producer = new Producer();
        Contract<Tag> contract = producer;
        Refined refined = producer;
        Method typed = Refined.class.getDeclaredMethod("create");
        Value first = new Value(), second = new Value();
        for (Fixed item : new Fixed[] {first,second,null,first}) {
            producer.selected = item;
            int before = producer.calls;
            same(producer.create(), item);
            same(contract.create(), item);
            same(refined.create(), item);
            same(typed.invoke(producer), item);
            check(producer.calls == before + 4);
            if (item != null) {
                Tag tag = new Tag(before);
                item.write(tag);
                same(((Result<Tag>) item).read(), tag);
                tag.value++;
                check(item.read().value == before + 1);
            }
        }
        Child child = new Child(); child.selected = second;
        same(((Contract<Tag>) child).create(), second);
        same(typed.invoke(child), second);
        check(child.calls == 4);
        RuntimeException sentinel = new IllegalStateException("owned");
        producer.failure = sentinel;
        for (int mode = 0; mode < 3; mode++) {
            int before = producer.calls;
            try {
                if (mode == 0) producer.create();
                else if (mode == 1) contract.create();
                else refined.create();
                throw new AssertionError();
            } catch (RuntimeException actual) { same(actual, sentinel); failures++; }
            check(producer.calls == before + 1);
        }
        boolean original = args[0].equals("original");
        int bridges = 0;
        for (Method method : Refined.class.getDeclaredMethods()) {
            if (method.getReturnType() == Result.class) {
                bridges++;
                check(method.isBridge() && method.isSynthetic() && method.isDefault());
            } else check(method.getReturnType() == Fixed.class && Modifier.isAbstract(method.getModifiers()));
        }
        check(bridges == (original ? 0 : 1));
        check(Refined.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Contract<fixture.Tag>"));
        check(Fixed.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Result<fixture.Tag>"));
        check(Contract.class.getDeclaredMethod("create").getGenericReturnType().getTypeName().equals("fixture.Result<P>"));
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println("full-nominal-return=retained,original-class-dispatch=matched,added-interface-slot=reported");
    }
}
