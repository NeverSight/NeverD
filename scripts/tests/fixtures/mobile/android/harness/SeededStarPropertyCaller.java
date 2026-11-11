import fixture.*;
import java.io.IOException;
public final class SeededStarPropertyCaller {
    public static <Z> Provider generic(String context,Z value,Token<Z,?> token) throws IOException { return Core.call(context,value,token); }
    public static Provider checked(String context,Object value,Token<String,?> token) throws IOException { return Core.checked(context,value,token); }
    public static Provider typed(String context,String value,Token<String,?> token) throws IOException { return Core.call(context,value,token); }
    public static <Z> Provider discard(String context,Z value,Token<Z,?> token) throws IOException { return Core.discard(context,value,token); }
}
