import fixture.*;
import java.io.IOException;
public final class AnchoredSeedNominalCaller {
    public static <Z> Provider generic(Token<Z> token,Z value) throws IOException { return Core.call(token,value); }
    public static Provider checked(Token<String> token,Object value) throws IOException { return Core.checked(token,value); }
    public static Provider typed(Token<String> token,String value) throws IOException { return Core.call(token,value); }
    public static <Z> Provider discard(Token<Z> token,Z value) throws IOException { return Core.discard(token,value); }
}
