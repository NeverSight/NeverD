package fixture;
import java.io.IOException;
public final class Core {
    private Core() {}
    public static Provider receiver(){Trace.receivers++;Trace.hit(7);Trace.before.run();return Trace.source;}
    public static <U> int call(U input)throws IOException {Context value=(Context)input;return receiver().read(value);}
    public static int checked(Object input)throws IOException {return call((Context)input);}
}
