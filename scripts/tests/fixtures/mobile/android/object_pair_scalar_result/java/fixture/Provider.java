package fixture;
public class Provider {
    public static <E> int score(E left,E right,boolean flag) {
        Trace.hit(1);Trace.left=left;Trace.right=right;Trace.flag=flag;
        Trace.hit(2);Trace.action.run();Trace.hit(3);return Trace.result;
    }
}
