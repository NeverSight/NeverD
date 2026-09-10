package fixture;

import java.util.List;

public final class GenericOps {
    public GenericOps() {}

    public static <U> U identity(U input) {
        return input;
    }

    public static <N extends Number & Comparable<N>> N intersection(N input) {
        return input;
    }

    public static <I extends CharSequence> I interfaceOnly(I input) {
        return input;
    }

    public static <U> List<? super U> lowerIdentity(List<? super U> input) {
        return input;
    }

    public static int scalar(int input) {
        return input * 3 + 7;
    }
}
