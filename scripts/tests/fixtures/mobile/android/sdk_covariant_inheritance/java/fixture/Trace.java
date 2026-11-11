package fixture;
public class Trace {
    public static Hook hook;
    public static int stage;
    public static int count;
    public static int events;
    public static void hit(int phase) throws Failure {
        stage=phase;
        count++;
        events=events*10+phase;
        if(hook!=null)hook.tick();
    }
}
