import fixture.Token;
public class NominalSuperConstructorProbe extends Token {
    public NominalSuperConstructorProbe(Object payload){super(payload);}
    public static final class Child extends NominalSuperConstructorProbe {
        public Child(Object payload){super(payload);}
    }
}
