import fixture.Callback;
import fixture.MarkerHost;

public class AnonymousMarkerCallbackHarness {
    public static void main(String[] args) throws Throwable {
        for (int value : new int[]{Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            if (MarkerHost.read(value) != value || MarkerHost.callback(value) != value + 9) {
                throw new AssertionError("Access constructor or callback behavior changed");
            }
            System.out.println(value);
        }
        Class<?> marker = Class.forName(args[0]);
        if (marker.getEnclosingClass() != MarkerHost.class
                || marker.getDeclaredFields().length != 0
                || marker.getEnclosingMethod() != null
                || marker.getDeclaredConstructors().length != 1
                || marker.getDeclaredConstructors()[0].getParameterCount() != 0
                || marker.getInterfaces().length != 1
                || marker.getInterfaces()[0] != Callback.class) {
            throw new AssertionError("Shared implementation/marker class changed");
        }
        Class<?> box = Class.forName("fixture.MarkerHost$Box");
        boolean markerUse = false;
        for (java.lang.reflect.Constructor<?> constructor : box.getDeclaredConstructors()) {
            Class<?>[] parameters = constructor.getParameterTypes();
            markerUse |= parameters.length == 2 && parameters[0] == int.class && parameters[1] == marker;
        }
        if (!markerUse) throw new AssertionError("Constructor lost the shared marker type");
        System.out.println("anonymous=" + marker.isAnonymousClass() + ",member=" + marker.isMemberClass());
    }
}
