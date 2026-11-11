import fixture.*;
import java.lang.reflect.*;
public class FloatingConstructorPrefixCaller {
    public static Core make(boolean chain,boolean simple,float a,float b,float e,double c,double d,Object context) throws Throwable {
        try {
            if(simple)return Core.class.getConstructor(float.class,float.class,float.class,int.class).newInstance(a,b,e,0);
            if(chain)return Core.class.getConstructor(float.class,float.class,float.class,double.class,double.class,Object.class).newInstance(a,b,e,c,d,context);
            return Core.class.getConstructor(float.class,float.class,double.class,double.class,Object.class).newInstance(a,b,c,d,context);
        }catch(InvocationTargetException failure){throw failure.getCause();}
    }
}
