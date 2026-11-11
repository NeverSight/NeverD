import fixture.MarkerHost;
import java.lang.reflect.Modifier;

public class AnonymousMarkerHarness {
    public static void main(String[] args) throws Throwable {
        for (int value : new int[]{Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            if (MarkerHost.read(value) != value) {
                throw new AssertionError("Constructor access marker changed the argument");
            }
            System.out.println(value);
        }
        Class<?> marker = Class.forName(args[0]);
        if (marker.getEnclosingClass() != MarkerHost.class
                || marker.getDeclaredFields().length != 0
                || marker.getEnclosingMethod() != null) {
            throw new AssertionError("Marker enclosure or empty field layout changed");
        }
        int constructors = marker.getDeclaredConstructors().length;
        if (constructors > 0 && (constructors != 1
                || !Modifier.isPrivate(marker.getDeclaredConstructors()[0].getModifiers()))) {
            throw new AssertionError("Source marker constructor must be a private helper");
        }
        System.out.println("anonymous=" + marker.isAnonymousClass()
                + ",member=" + marker.isMemberClass() + ",constructors=" + constructors);
    }
}
