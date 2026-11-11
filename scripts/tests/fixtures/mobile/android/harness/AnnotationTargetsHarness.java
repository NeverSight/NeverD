import fixture.ModuleMarker;
import fixture.RecordMarker;
import java.lang.annotation.Target;
import java.util.Arrays;

public class AnnotationTargetsHarness {
    public static void main(String[] args) throws Exception {
        for (Class<?> annotation : new Class<?>[]{ModuleMarker.class, RecordMarker.class}) {
            System.out.println(annotation.getName() + ":" +
                Arrays.toString(annotation.getAnnotation(Target.class).value()) + ":" +
                annotation.getDeclaredMethod("value").getDefaultValue());
        }
    }
}
