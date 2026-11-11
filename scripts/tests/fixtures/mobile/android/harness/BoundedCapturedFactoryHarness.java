import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class BoundedCapturedFactoryHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned bounded captured factory contract"); }
    static Object call(int route, Outer outer, Converter converter, String label, int mode) throws Throwable {
        if(route==0) return Caller.call(outer,converter,label,mode);
        if(route==1) return BoundedCapturedFactoryCaller.call(outer,converter,label,mode);
        if(route==2) {
            try { return Caller.class.getMethod("call",Outer.class,Converter.class,String.class,int.class).invoke(null,outer,converter,label,mode); }
            catch(InvocationTargetException failure){throw failure.getCause();}
        }
        return MethodHandles.publicLookup().findStatic(Caller.class,"call",
            MethodType.methodType(Outer.Member.class,Outer.class,Converter.class,String.class,int.class)).invoke(outer,converter,label,mode);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        boolean projected=args[2].equals("source");
        require(Modifier.isStatic(Outer.Member.class.getModifiers())==projected);
        Field capture=Outer.Member.class.getDeclaredField("this$0"); capture.setAccessible(true);
        require(capture.isSynthetic()!=projected);
        Field valueField=Outer.Member.class.getDeclaredField("value"); valueField.setAccessible(true);
        Field vectorField=Outer.Member.class.getDeclaredField("vector"); vectorField.setAccessible(true);
        int[] cells={7}; Vector vector=mode==10?null:new ConcreteVector(cells);
        Object value=mode==9?null:new Object();
        int[] valueCalls={0}, convertCalls={0};
        Converter<Object,Vector> converter=mode==11?null:new Converter<Object,Vector>() {
            public Object value(){ valueCalls[0]++; Trace.hit(2); return value; }
            public Vector convert(Object argument){
                require(argument==value); convertCalls[0]++; Trace.hit(3);
                if(vector!=null) vector.cells[0]++; return vector;
            }
        };
        Outer<Object> parent=mode==8?null:new Outer<Object>(new Object());
        Outer<Object> other=new Outer<Object>(new Object());
        if(parent!=null) parent.destination=mode==13?null:other;
        int selector=mode==12 || mode==13 ? 1 : 0;
        Object chosen=selector==0?parent:parent.destination;
        Trace.failure=new IllegalArgumentException("owned captured failure");
        Trace.fail=mode>=1 && mode<=7?mode:0;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0; Object prior=new Object(); Trace.last=prior; Trace.captured=null;
            int oldValue=valueCalls[0], oldConvert=convertCalls[0], oldCells=cells[0];
            Object result=null; Throwable failure=null;
            try { result=call(route,parent,converter,mode==14?null:"owned",selector); } catch(Throwable caught){failure=caught;}
            boolean calledValue=mode!=1 && mode!=11;
            boolean calledConvert=calledValue && mode!=2;
            boolean converted=calledConvert && mode!=3;
            boolean allocated=converted && mode!=4 && mode!=8 && mode!=13;
            boolean assigned=allocated && mode!=6;
            boolean stored=assigned && mode!=7;
            long expected=!calledValue?1:!calledConvert?12:!converted?123:!allocated?1234:!assigned?12346:!stored?123467:1234675;
            if(mode>=1 && mode<=7) require(failure==Trace.failure);
            else if(mode==8 || mode==11 || mode==13) require(failure instanceof NullPointerException);
            else require(failure==null && result==Trace.captured);
            require(Trace.events==expected && valueCalls[0]==oldValue+(calledValue?1:0) && convertCalls[0]==oldConvert+(calledConvert?1:0));
            require(cells[0]==oldCells+(converted && vector!=null?1:0));
            require(Trace.last==(stored?Trace.captured:prior));
            if(allocated) {
                require(Trace.captured!=null && capture.get(Trace.captured)==chosen);
                require(valueField.get(Trace.captured)==(assigned?value:null));
                require(vectorField.get(Trace.captured)==(assigned?vector:null));
            } else require(Trace.captured==null);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
