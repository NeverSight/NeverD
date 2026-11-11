package fixture;
public final class Core {
    public final Value first, second;
    public final double carry;
    public Core(Value first,Value second,double carry) {
        Trace.partial=this;
        Trace.step(5);
        this.first=first;this.second=second;this.carry=carry;
        Trace.step(6);
    }
    public Core(double a,double b,double c,double d,double e,double f,boolean firstInput,boolean secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,byte firstInput,byte secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,char firstInput,char secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,short firstInput,short secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,int firstInput,int secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,long firstInput,long secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,float firstInput,float secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,double firstInput,double secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,Object firstInput,Object secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
    public Core(double a,double b,double c,double d,double e,double f,Object[] firstInput,Object[] secondInput) { this(new Value(firstInput),new Value(secondInput),a); }
}
