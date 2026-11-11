import fixture.Entry;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public final class LocalSetterCaller {
    public static Object exchange(java.util.Map.Entry<Object,Object> entry, Object input) {
        return entry.setValue(input);
    }
    public static void write(Entry<Object,Object> entry, Object input, String name) throws Throwable {
        MethodHandles.publicLookup().findVirtual(Entry.class, name,
                MethodType.methodType(void.class, Object.class)).invokeExact(entry, input);
    }
}
