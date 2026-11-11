package fixture;
import java.io.IOException;
public final class Core {
    private Core() {}
    public static Target make(Holder holder, boolean flag) throws IOException {
        return new Target(holder.first.length - 2 > 0 ? holder.first.length - 2 : holder.first.length,
                          flag ? holder.second.length : Trace.read(holder));
    }
}
