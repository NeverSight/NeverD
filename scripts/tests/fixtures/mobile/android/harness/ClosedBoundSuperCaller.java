import fixture.Child;
import fixture.Service;
public final class ClosedBoundSuperCaller {
    public static Child create(ClassLoader loader,Service manager) {return new Child(loader,manager);}
}
