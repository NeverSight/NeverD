import fixture.*;
import java.util.*;
public final class AllocatedConstructorContextCaller {
    public static Target<String> fresh() { return new Target<String>(); }
    public static Target<String> given(List<Comparator<String>> values,Context context) { return new Target<String>(values,context); }
}
