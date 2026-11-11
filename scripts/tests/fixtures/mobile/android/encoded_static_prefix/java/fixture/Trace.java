package fixture;
public class Trace {
    public static int seedA,fail;
    public static long seedB,events;
    public static float seedC;
    public static double seedD;
    public static boolean reenter;
    public static Core partial,nested;
    public static Throwable failure;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value) {if(failure instanceof RuntimeException)throw (RuntimeException)failure;throw (Error)failure;}
    }
    public static int seed() {step(1);return seedA;}
    public static long wide() {
        step(2);
        if(reenter) {reenter=false;nested=new Core(99,98L,97.0f,96.0d,null,15);}
        return seedB;
    }
    public static float single() {step(3);return seedC;}
    public static double real() {step(4);return seedD;}
    public static Object token(Object value) {step(5);return value;}
}
