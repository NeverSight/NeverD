package fixture;
import java.io.IOException;
public final class Core {
    private Core() {}
    public static Provider receiver(){Trace.receivers++;Trace.hit(7);Trace.before.run();return Trace.source;}
    public static <T> Context call(T value,boolean left,boolean right)throws IOException {String context=Trace.nextContext;return receiver().read(context,value,left,right);}
    public static Context checked(Object value,boolean left,boolean right)throws IOException {return call((Context)value,left,right);}
}
