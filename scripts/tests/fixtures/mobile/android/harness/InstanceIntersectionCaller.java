import fixture.*;
public class InstanceIntersectionCaller {
    public static void run(Core core, boolean contract) {
        if (contract) core.runInterface();
        else core.runVirtual();
    }
}
