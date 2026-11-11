import fixture.BoundBox;
import java.lang.reflect.Method;
import java.lang.reflect.TypeVariable;

public class GenericBoundsHarness {
    public static void main(String[] args) throws Exception {
        BoundBox<CharSequence, CharSequence> box = new BoundBox<>();
        CharSequence[] values = {null, "", "value", new StringBuilder("builder")};
        int probes = 0;
        for (CharSequence left : values)
            for (CharSequence right : values)
                for (boolean first : new boolean[] {false, true}) {
                    if (box.select(first, left, right) != (first ? left : right))
                        throw new AssertionError("Bound substitution changed returned identity");
                    probes++;
                }
        TypeVariable<?>[] formals = BoundBox.class.getTypeParameters();
        Method method = BoundBox.class.getMethod("select", boolean.class, CharSequence.class, CharSequence.class);
        TypeVariable<?>[] local = method.getTypeParameters();
        if (formals.length != 2 || !formals[0].getBounds()[0].equals(formals[1])
                || formals[1].getBounds()[0] != CharSequence.class
                || local.length != 1 || !local[0].getBounds()[0].equals(formals[0])
                || !method.getGenericReturnType().equals(local[0])
                || !method.getGenericParameterTypes()[1].equals(local[0])
                || !method.getGenericParameterTypes()[2].equals(local[0]))
            throw new AssertionError("Generic bound ownership or method substitution changed");
        System.out.println("identity-probes=" + probes);
        System.out.println("V:" + formals[0].getBounds()[0].getTypeName()
                + ";W:" + formals[1].getBounds()[0].getTypeName()
                + ";T:" + local[0].getBounds()[0].getTypeName());
    }
}
