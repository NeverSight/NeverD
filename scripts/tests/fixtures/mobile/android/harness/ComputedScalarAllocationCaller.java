import fixture.*;
import java.util.*;
public final class ComputedScalarAllocationCaller {
    public static Target<String> computed(List<Comparator<String>> values) { return new Target<String>(values); }
    public static Target<String> given(List<Comparator<String>> values,Context context) { return new Target<String>(values,context); }
}
