import fixture.LocalClassBehavior;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class LocalClassHarness {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void inspect(String role, String binaryName) throws Exception {
        Class<?> type = Class.forName(binaryName);
        require(type.isLocalClass() && !type.isAnonymousClass() && !type.isMemberClass(), role + " is not local");
        require(type.getDeclaringClass() == null && type.getCanonicalName() == null, role + " acquired member identity");
        require(type.getSimpleName().equals("Worker"), role + " has the wrong source name");
        require(type.getSuperclass() == Object.class && type.getInterfaces().length == 0, role + " changed parents");
        require(type.getDeclaredFields().length == 0 && Modifier.isFinal(type.getModifiers()), role + " acquired storage or changed access");
        Method enclosing = type.getEnclosingMethod();
        require(enclosing != null && enclosing.getDeclaringClass() == LocalClassBehavior.class
                && Modifier.isStatic(enclosing.getModifiers()), role + " changed enclosing owner");
        Class<?>[] parameters = enclosing.getParameterTypes();
        if (role.equals("first-long")) {
            require(enclosing.getName().equals("first") && parameters.length == 1
                    && parameters[0] == long.class && enclosing.getReturnType() == long.class,
                    "wide overload changed");
        } else {
            String name = role.equals("first-int") ? "first" : "second";
            require(enclosing.getName().equals(name) && parameters.length == 2
                    && parameters[0] == int.class && parameters[1] == int.class
                    && enclosing.getReturnType() == int.class, role + " overload changed");
        }
        Constructor<?>[] constructors = type.getDeclaredConstructors();
        require(constructors.length == 1 && constructors[0].getParameterTypes().length == 0,
                role + " constructor captures arguments");
        System.out.println("reflection:" + role + "=1");
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "expected independently measured binary-name manifest");
        List<String> lines = Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8);
        Map<String, String> names = new HashMap<String, String>();
        for (String line : lines) {
            String[] parts = line.split("\t", -1);
            require(parts.length == 2 && !parts[1].isEmpty() && names.put(parts[0], parts[1]) == null,
                    "invalid or duplicate manifest row");
        }
        require(names.size() == 3 && names.containsKey("first-int") && names.containsKey("second-int")
                && names.containsKey("first-long"), "incomplete reflection roles");
        for (String role : new String[] {"first-int", "second-int", "first-long"}) inspect(role, names.get(role));
        System.out.println("constant-reflection=" + LocalClassBehavior.class.getField("CONSTANT").getInt(null));
        int[] values = {Integer.MIN_VALUE, -17, -1, 0, 1, 17, Integer.MAX_VALUE};
        int[] arguments = {-1, 0, 1, 2, 3, 7, 8};
        for (int i = 0; i < values.length; i++) {
            for (int j = 0; j < arguments.length; j++) {
                System.out.println("first:" + i + ":" + j + "=" + LocalClassBehavior.first(values[i], arguments[j]));
                System.out.println("first-count:" + i + ":" + j + "=" + LocalClassBehavior.firstCount);
                System.out.println("second:" + i + ":" + j + "=" + LocalClassBehavior.second(values[i], arguments[j]));
                System.out.println("second-count:" + i + ":" + j + "=" + LocalClassBehavior.secondCount);
            }
        }
        long[] wide = {Long.MIN_VALUE, -17L, -1L, 0L, 1L, 17L, Long.MAX_VALUE};
        for (int i = 0; i < wide.length; i++) {
            System.out.println("wide:" + i + "=" + LocalClassBehavior.first(wide[i]));
            System.out.println("wide-count:" + i + "=" + LocalClassBehavior.wideCount);
        }
        System.out.println("final-first=" + LocalClassBehavior.firstCount);
        System.out.println("final-second=" + LocalClassBehavior.secondCount);
        System.out.println("final-wide=" + LocalClassBehavior.wideCount);
    }
}
