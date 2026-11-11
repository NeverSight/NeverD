import fixture.Left;
import fixture.Right;
public final class GuardedClosedDelegationProbe {
    public static final class LeftChild extends Left {
        public LeftChild(Object payload){super(payload);}
    }
    public static final class RightChild extends Right {
        public RightChild(Object payload){super(payload);}
    }
}
