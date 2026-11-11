import fixture.Core;
import java.io.IOException;

public final class ArrayLengthConstructorPrefixCaller {
    public static Core create(byte[] input, int start, int length, int kind) throws IOException {
        return kind==0?new Core(input):new Core(input,start,length);
    }
}
