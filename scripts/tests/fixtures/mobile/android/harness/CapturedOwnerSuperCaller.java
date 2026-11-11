import fixture.*;
import java.lang.reflect.InvocationTargetException;
public class CapturedOwnerSuperCaller {
    public static Tree.Node make(Tree outer, float start, float end, Object data, int color) throws Throwable {
        try { return (Tree.Node)Tree.Node.class.getDeclaredConstructor(Tree.class,float.class,float.class,Object.class,int.class)
                .newInstance(outer,start,end,data,color); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
}
