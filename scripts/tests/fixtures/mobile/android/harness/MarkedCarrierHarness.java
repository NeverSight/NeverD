import fixture.Carrier;
import fixture.Marker;
import fixture.RootMarker;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.util.Arrays;
import java.util.List;

public final class MarkedCarrierHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Marked carrier contract changed");
    }

    public static void main(String[] args) throws Exception {
        Method use = Carrier.class.getDeclaredMethod("use", Object.class, List.class);
        require(Modifier.isPrivate(use.getModifiers()) && Modifier.isStatic(use.getModifiers()));
        use.setAccessible(true);
        List<Object> values = Arrays.asList(new Object(), "entry");
        Object companion = Carrier.companion;
        require(companion != null && Carrier.calls == 0);
        int index = 0;
        for (Object token : new Object[] {null, new Object(), "token", Long.valueOf(Long.MIN_VALUE)}) {
            Carrier first = Carrier.wrap(token), second = Carrier.wrap(token);
            require(first != second && first.unwrap() == token && second.unwrap() == token);
            require(first instanceof Marker && first instanceof RootMarker);
            require(use.invoke(null, token, values) == token && use.invoke(null, token, null) == token);
            require(values.size() == 2 && Carrier.companion == companion && Carrier.calls == 2 * (index + 1));
            System.out.println("identity:" + index++);
        }
        Carrier absent = null;
        try {
            absent.unwrap();
            throw new AssertionError("null wrapper did not fail");
        } catch (NullPointerException expected) { System.out.println("null:matched"); }
        require(Carrier.calls == 8 && Carrier.companion == companion);
        ParameterizedType list = (ParameterizedType) use.getGenericParameterTypes()[1];
        Type argument = list.getActualTypeArguments()[0];
        if (argument instanceof TypeVariable)
            require(((TypeVariable<?>) argument).getGenericDeclaration() == Carrier.class);
        require(Carrier.class.getTypeParameters().length == 1
                && Carrier.class.getTypeParameters()[0].getBounds()[0] == Object.class);
        System.out.println("generic=" + argument.getTypeName());
    }
}
