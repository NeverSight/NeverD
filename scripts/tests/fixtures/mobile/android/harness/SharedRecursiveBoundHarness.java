import fixture.Comparisons;
import fixture.MutableSelector;
import fixture.Owner;
import fixture.Rank;
import fixture.Reader;
import fixture.Selector;
import fixture.WideRank;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class SharedRecursiveBoundHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("Shared recursive bound behavior changed");
    }
    private static Type argument(Type type, int index) { return ((ParameterizedType) type).getActualTypeArguments()[index]; }
    private static void bound(Method method, int index) {
        TypeVariable<?> formal = method.getTypeParameters()[index];
        Type type = formal.getBounds()[0];
        require(formal.getBounds().length == 1 && ((ParameterizedType) type).getRawType() == Comparable.class);
        WildcardType wildcard = (WildcardType) argument(type, 0);
        require(wildcard.getLowerBounds().length == 1 && wildcard.getLowerBounds()[0].equals(formal));
    }
    private static void projected(Type type) {
        require(((ParameterizedType) type).getRawType() == Comparable.class);
        WildcardType wildcard = (WildcardType) argument(type, 0);
        require(wildcard.getLowerBounds().length == 0 && wildcard.getUpperBounds().length == 1
                && wildcard.getUpperBounds()[0] == Object.class);
    }
    public static void main(String[] args) throws Exception {
        int probes = 0, echoes = 0, failures = 0;
        Class<?> worker = null;
        for (int number : new int[] {-1, 0, 1}) {
            for (int factory = 0; factory < 2; ++factory) {
                Rank key = new Rank(0);
                Rank value = new Rank(number);
                MutableSelector selector = new MutableSelector(value);
                Reader reader = (Reader) (factory == 0 ? Owner.make(selector, key) : Owner.another(selector, key));
                if (worker != null) require(worker == reader.getClass());
                worker = reader.getClass();
                Object input = new Object();
                require(reader.read(input) == Integer.valueOf(number) && selector.last == input); ++probes;
                Method typed = null;
                for (Method method : worker.getDeclaredMethods())
                    if (method.getName().equals("read") && !method.isBridge()) typed = method;
                require(typed != null && typed.getReturnType() == Integer.class);
                require(typed.invoke(reader, input) == Integer.valueOf(number) && selector.calls == 2); ++probes;
                Method echo = worker.getDeclaredMethod("echo", Object.class);
                require(echo.invoke(reader, input) == input && selector.calls == 2); ++echoes;
                require(echo.getTypeParameters().length == 1 && echo.getGenericReturnType().equals(echo.getTypeParameters()[0]));
                value.value = -number;
                require(reader.read(null) == Integer.valueOf(-number) && selector.last == null); ++probes;
                selector.result = null;
                require(reader.read(input) == Integer.valueOf(-1)); ++probes;
                selector.result = key;
                require(reader.read(input) == Integer.valueOf(0) && key.calls == 0); ++probes;
                RuntimeException sentinel = new RuntimeException("owned selector failure");
                selector.failure = sentinel;
                int before = selector.calls;
                try { reader.read(input); throw new AssertionError("missing selector failure"); }
                catch (RuntimeException actual) { require(actual == sentinel && selector.calls == before + 1 && selector.last == input); ++failures; }
                selector.failure = null;
                selector.result = value;
                value.failure = sentinel;
                before = value.calls;
                try { reader.read(input); throw new AssertionError("missing comparison failure"); }
                catch (RuntimeException actual) { require(actual == sentinel && value.calls == before + 1); ++failures; }
                value.failure = null;
                selector.result = "owned invalid selected value";
                before = selector.calls;
                try { reader.read(input); throw new AssertionError("missing selected Comparable cast"); }
                catch (ClassCastException expected) { require(selector.calls == before + 1); ++failures; }
                Reader empty = (Reader) Owner.make(null, key);
                try { empty.read(input); throw new AssertionError("missing null selector failure"); }
                catch (NullPointerException expected) { ++failures; }
                selector.result = value;
                WideRank wide = new WideRank(1);
                Reader mismatch = (Reader) Owner.make(selector, wide);
                before = value.calls;
                try { mismatch.read(input); throw new AssertionError("missing comparison bridge cast"); }
                catch (ClassCastException expected) { require(value.calls == before); ++failures; }
                wide.expected = key;
                selector.result = wide;
                require(reader.read(input) == Integer.valueOf(1) && wide.calls == 1); ++probes;
                wide.failure = sentinel;
                try { reader.read(input); throw new AssertionError("missing wide comparison failure"); }
                catch (RuntimeException actual) { require(actual == sentinel && wide.calls == 2); ++failures; }
                selector.result = value;
                Reader nullKey = (Reader) Owner.make(selector, null);
                before = value.calls;
                require(nullKey.read(input) == Integer.valueOf(1) && value.calls == before); ++probes;
                selector.result = null;
                require(nullKey.read(input) == Integer.valueOf(0)); ++probes;
            }
        }
        try { ((Reader) null).read(new Object()); throw new AssertionError("missing null reader failure"); }
        catch (NullPointerException expected) { ++failures; }
        Method make = Owner.class.getDeclaredMethod("make", Selector.class, Comparable.class);
        Method another = Owner.class.getDeclaredMethod("another", Selector.class, Comparable.class);
        bound(make, 1); bound(another, 1);
        bound(Comparisons.class.getDeclaredMethod("compare", Comparable.class, Comparable.class), 0);
        Constructor<?> constructor = worker.getDeclaredConstructor(Selector.class, Comparable.class);
        Type key = worker.getDeclaredField("key").getGenericType();
        Type selected = argument(worker.getDeclaredField("selector").getGenericType(), 1);
        Type selectedUpper = ((WildcardType) selected).getUpperBounds()[0];
        Type constructed = constructor.getGenericParameterTypes()[1];
        if (args[0].equals("original")) {
            require(worker.isAnonymousClass() && worker.getEnclosingMethod().equals(make));
            require(key.equals(make.getTypeParameters()[1]) && selectedUpper.equals(key) && constructed.equals(key));
        } else {
            require(worker.isMemberClass() && worker.getEnclosingMethod() == null);
            projected(key); projected(selectedUpper); projected(constructed);
        }
        System.out.println("comparison-identities=" + probes + ";echo-identities=" + echoes + ";ordered-failures=" + failures);
    }
}
