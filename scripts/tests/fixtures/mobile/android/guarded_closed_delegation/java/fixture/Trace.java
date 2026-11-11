package fixture;
import java.util.List;
public final class Trace {
    private Trace(){}
    public static Object parentPartial,corePartial,firstRead,secondRead,firstSeen,secondSeen;
    public static List<? extends Left> firstValue;
    public static List<? extends Right> secondValue;
    public static long wideValue,wideRead,wideSeen,events;
    public static float scaleValue,scaleRead,scaleSeen;
    public static int fail;
    public static Throwable failure;
    public static void step(int value){
        events=events*10+value;
        if(fail==value){
            if(failure instanceof RuntimeException)throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
    public static List<? extends Left> first(){
        List<? extends Left> value=firstValue;firstRead=value;step(1);return value;
    }
    public static List<? extends Right> second(){
        List<? extends Right> value=secondValue;secondRead=value;step(2);return value;
    }
    public static long wide(){long value=wideValue;wideRead=value;step(3);return value;}
    public static float scale(){float value=scaleValue;scaleRead=value;step(4);return value;}
}
