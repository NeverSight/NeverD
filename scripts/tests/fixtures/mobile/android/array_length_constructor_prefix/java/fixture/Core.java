package fixture;

import java.io.IOException;

public class Core {
    public final byte[] data;
    public final int start;
    public final int length;
    public Core(byte[] input) throws IOException {
        this(input, Trace.probe.begin(), input.length);
        Trace.probe.after(this);
    }
    public Core(byte[] input, int start, int length) throws IOException {
        this.data = input;
        this.start = start;
        this.length = length;
        Trace.probe.constructed(this);
    }
}
