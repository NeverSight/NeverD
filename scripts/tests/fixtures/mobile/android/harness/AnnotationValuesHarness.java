import fixture.Annotated;
import fixture.Values;
import java.lang.annotation.Annotation;
import java.util.Arrays;

public class AnnotationValuesHarness {
    private static void emit(String site, Values value) {
        if (value == null) throw new AssertionError("missing annotation: " + site);
        System.out.println(site + ":" + value.number() + ":" + value.active()
                + ":" + value.wide() + ":" + Float.floatToRawIntBits(value.negativeZero())
                + ":" + Double.doubleToRawLongBits(value.fraction())
                + ":" + Arrays.toString(value.version())
                + ":" + Arrays.toString(value.data()) + ":" + value.nested().name());
    }

    public static void main(String[] arguments) throws Exception {
        emit("class", Annotated.class.getAnnotation(Values.class));
        emit("field", Annotated.class.getField("view").getAnnotation(Values.class));
        emit("constructor", Annotated.class.getConstructor(Object.class).getAnnotation(Values.class));
        java.lang.reflect.Method method = Annotated.class.getMethod("identity", int.class);
        emit("method", method.getAnnotation(Values.class));
        for (Annotation annotation : method.getParameterAnnotations()[0])
            if (annotation instanceof Values) emit("parameter", (Values) annotation);
        Object view = new Object();
        Annotated instance = new Annotated(view);
        if (instance.view != view) throw new AssertionError("constructor changed reference");
        System.out.println("defaults:" + Arrays.toString(Annotated.DEFAULTS));
        for (int value : new int[]{Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE})
            System.out.println("identity:" + instance.identity(value));
        System.out.println("final:" + java.lang.reflect.Modifier.isFinal(
                Annotated.class.getField("view").getModifiers()));
    }
}
