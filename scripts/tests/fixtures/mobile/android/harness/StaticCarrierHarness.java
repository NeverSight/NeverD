import fixture.Carrier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.util.Arrays;
import java.util.List;

public class StaticCarrierHarness {
    public static void main(String[] args) throws Exception {
        List<Object> values = Arrays.asList(new Object(), "entry");
        int index = 0;
        for (Object token : new Object[] {null, new Object(), "token", Long.valueOf(Long.MAX_VALUE)}) {
            Carrier first = Carrier.wrap(token);
            Carrier second = Carrier.wrap(token);
            if (first == second || first.unwrap() != token || second.unwrap() != token
                    || Carrier.use(token, values) != token || Carrier.use(token, null) != token
                    || values.size() != 2)
                throw new AssertionError("wrapper or static carrier changed reference identity");
            System.out.println("identity:" + index++);
        }
        Carrier absent = null;
        try {
            absent.unwrap();
            throw new AssertionError("null wrapper did not fail");
        } catch (NullPointerException expected) {
            System.out.println("null:matched");
        }
        ParameterizedType list = (ParameterizedType) Carrier.class
                .getDeclaredMethod("use", Object.class, List.class).getGenericParameterTypes()[1];
        Type argument = list.getActualTypeArguments()[0];
        if (argument instanceof TypeVariable
                && ((TypeVariable<?>) argument).getGenericDeclaration() != Carrier.class)
            throw new AssertionError("static metadata variable lost its declaring class");
        if (Carrier.class.getTypeParameters().length != 1
                || Carrier.class.getTypeParameters()[0].getBounds()[0] != Object.class)
            throw new AssertionError("original class formal was changed");
        System.out.println("generic=" + argument.getTypeName());
    }
}
