import fixture.*;
import java.lang.reflect.*;

public final class SdkInterfaceFieldsHarness {
    public static void main(String[] args) throws Throwable {
        if (State.starts != 0) throw new AssertionError("dependency initialized before original use");
        OriginalRawParametersHarness.main(args);
        if (State.starts != 1) throw new AssertionError("original reflected-call initialization changed");
        Field flags = Api.class.getField("FLAGS");
        Field label = Api.class.getField("LABEL");
        Field state = Api.class.getField("STATE");
        for (Field field : new Field[] {flags, label, state}) {
            int access = field.getModifiers();
            if (field.getDeclaringClass() != Api.class || !Modifier.isPublic(access)
                    || !Modifier.isStatic(access) || !Modifier.isFinal(access))
                throw new AssertionError("dependency field roles or ownership changed");
        }
        if (flags.getInt(null) != 7 || !label.get(null).equals("owned-sdk"))
            throw new AssertionError("dependency constant values changed");
        State original = Api.STATE;
        if (State.starts != 1 || original != state.get(null) || original != Api.STATE
                || original.marker != 7)
            throw new AssertionError("dependency initialization or field identity changed");
        original.marker = 19;
        if (((State) state.get(null)).marker != 19 || Api.STATE.marker != 19 || State.starts != 1)
            throw new AssertionError("dependency field alias mutation changed");
        System.out.println("fields=3,constant-values=2,initializations=1,mutable-sdk-state=matched");
    }
}
