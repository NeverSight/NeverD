import fixture.View;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public final class CapturedSetterCaller {
    public static Object exchange(java.util.Map.Entry<Object,Object> entry, Object input) {
        return entry.setValue(input);
    }
    public static void write(View<Object,Object> entry, Object input, String name) throws Throwable {
        java.lang.reflect.Method method=entry.getClass().getDeclaredMethod(name,Object.class);
        method.setAccessible(true);
        MethodHandles.lookup().unreflect(method).invoke(entry,input);
    }
}
