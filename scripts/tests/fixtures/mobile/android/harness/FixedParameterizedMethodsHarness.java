import fixture.*;
import java.lang.reflect.*;

public class FixedParameterizedMethodsHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static void put(int entry, FixedDirect direct, FixedOwner owner, FixedBox<? super FixedTag> box) {
        if (entry == 0) direct.put(box);
        else if (entry == 1) ((FixedApi) direct).put(box);
        else if (entry == 2) owner.put(box);
        else if (entry == 3) ((FixedChild) owner).put(box);
        else ((FixedApi) owner).put(box);
    }
    static FixedBox<? super FixedTag> get(int entry, FixedDirect direct, FixedOwner owner) {
        if (entry == 0) return direct.get();
        if (entry == 1) return ((FixedApi) direct).get();
        if (entry == 2) return owner.get();
        if (entry == 3) return ((FixedChild) owner).get();
        return ((FixedApi) owner).get();
    }
    static void parameterized(Type type) {
        check(type instanceof ParameterizedType);
        ParameterizedType box = (ParameterizedType) type;
        same(box.getRawType(), FixedBox.class);
        check(box.getActualTypeArguments().length == 1);
        Type argument = box.getActualTypeArguments()[0];
        check(argument instanceof WildcardType);
        WildcardType wildcard = (WildcardType) argument;
        check(wildcard.getLowerBounds().length == 1);
        same(wildcard.getLowerBounds()[0], FixedTag.class);
        same(wildcard.getUpperBounds()[0], Object.class);
    }
    public static void main(String[] args) throws Exception {
        FixedDirect direct = new FixedDirect();
        FixedOwner owner = new FixedOwner();
        FixedTag tag = new FixedTag(-7);
        FixedBox<Object> broad = new FixedBox<Object>(); broad.value = tag;
        FixedBox<FixedTag> narrow = new FixedBox<FixedTag>(); narrow.value = tag;
        FixedBox<?>[] boxes = {broad, narrow, null};
        for (int entry = 0; entry < 5; entry++) for (FixedBox<?> value : boxes) {
            @SuppressWarnings("unchecked") FixedBox<? super FixedTag> box = (FixedBox<? super FixedTag>) value;
            direct.calls = 0; owner.calls = 0;
            put(entry, direct, owner, box);
            same(get(entry, direct, owner), box);
            same(entry < 2 ? direct.selected : owner.selected, box);
            check(direct.calls == (entry < 2 ? 1 : 0));
            check(owner.calls == (entry < 2 ? 0 : 1));
            if (box != null) same(box.value, tag);
        }
        for (int entry = 0; entry < 5; entry++) {
            try { put(entry, null, null, broad); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { failures++; }
            try { get(entry, null, null); throw new AssertionError("missing null getter receiver"); }
            catch (NullPointerException expected) { failures++; }
        }
        for (Object input : new Object[] {tag, broad, "owned", null}) {
            same(owner.echo(input), input); same(((FixedChild) owner).echo(input), input);
        }
        for (Class<?> declaration : new Class<?>[] {FixedApi.class, FixedChild.class, FixedDirect.class, FixedOwner.class}) {
            check(declaration.getTypeParameters().length == 0);
            Method put = declaration.getMethod("put", FixedBox.class);
            check(put.getTypeParameters().length == 0);
            parameterized(put.getGenericParameterTypes()[0]);
            same(put.getReturnType(), void.class);
            parameterized(declaration.getMethod("get").getGenericReturnType());
        }
        for (Class<?> declaration : new Class<?>[] {FixedChild.class, FixedOwner.class}) {
            Method echo = declaration.getDeclaredMethod("echo", Object.class);
            TypeVariable<?> formal = echo.getTypeParameters()[0];
            same(formal.getGenericDeclaration(), echo); same(formal.getBounds()[0], Object.class);
            check(echo.getGenericParameterTypes()[0].equals(formal));
            check(echo.getGenericReturnType().equals(formal));
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("fixed-parameterized-methods-and-variance=retained,direct-and-inherited-dispatch-and-effects=matched");
    }
}
