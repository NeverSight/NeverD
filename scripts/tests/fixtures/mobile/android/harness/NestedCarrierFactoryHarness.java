import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class NestedCarrierFactoryHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned nested carrier factory"); }
    static final class Trap implements Comparator<String> {
        public int compare(String left,String right) { throw new AssertionError("owned nested carrier factory eager compare"); }
    }
    static final class Input extends ArrayList<Comparator<String>> {
        public int size() { throw new AssertionError("owned nested carrier factory eager size"); }
        public Comparator<String> get(int index) { throw new AssertionError("owned nested carrier factory eager get"); }
        public Iterator<Comparator<String>> iterator() { throw new AssertionError("owned nested carrier factory eager iterator"); }
    }
    static final class ReadList extends ArrayList<Comparator<String>> {
        final RuntimeException failure;final boolean fail;
        ReadList(RuntimeException failure,boolean fail) {this.failure=failure;this.fail=fail;}
        public Comparator<String> get(int index) { Trace.hit(5);if(fail)throw failure;return super.get(index); }
    }
    public static final class Future extends Factory<String> {
        final Object tag=new Object();public Future(List<Comparator<String>> values) {super(values);}
    }
    static Object call(int route,int kind,List<Comparator<String>> source) throws Throwable {
        if(route==0)return kind==0?Client.call(source):Client.read(source);
        if(route==1)return kind==0?NestedCarrierFactoryCaller.call(source):NestedCarrierFactoryCaller.read(source);
        if(route==2) {
            try {return Client.class.getMethod(kind==0?"call":"read",List.class).invoke(null,source);}
            catch(InvocationTargetException failure) {throw failure.getCause();}
        }
        if(kind==0)return (Factory)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Factory.class,List.class)).invokeExact((List)source);
        return (Comparator)MethodHandles.publicLookup().findStatic(Client.class,"read",MethodType.methodType(Comparator.class,List.class)).invokeExact((List)source);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final List<Comparator<String>> input=mode==11?null:new Input();
        final Object element=mode==7?new Object():new Trap();
        final RuntimeException readFailure=new RuntimeException("owned list failure"),runtimeFailure=new RuntimeException("owned callback failure");
        final Error errorFailure=new Error("owned callback error");
        final ReadList returnedValues=mode==8?null:new ReadList(readFailure,mode==9);
        if(returnedValues!=null)((List)returnedValues).add(element);
        final Factory<String> initial=new Factory<String>(new ArrayList<Comparator<String>>());
        final Future replacement=new Future(returnedValues);
        final Object wanted=mode==5?null:mode==6?new Object():mode==10?initial:replacement;
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==1?2:mode==2?3:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();Trace.events=0;Trace.input=sentinel;Trace.result=initial;
            final int created=Trace.created;
            Trace.action=new Runnable() {public void run() {
                require(Trace.events==2&&Trace.input==input);Trace.hit(4);Trace.result=wanted;
                if(mode==3)throw runtimeFailure;if(mode==4)throw errorFailure;
            }};
            Throwable failure=null;Object actual=sentinel;
            try {actual=call(route,kind,input);}catch(Throwable error) {failure=error;}
            boolean early=mode==1||mode==2||mode==3||mode==4;
            boolean reads=kind==1&&!early&&mode!=5&&mode!=6&&mode!=8&&mode!=10;
            long events=mode==1?2:mode==3||mode==4?24:reads?2435:243;
            require(Trace.events==events&&Trace.created==created&&Trace.input==(mode==1?sentinel:input));
            Throwable expected=mode==1||mode==2?Trace.failure:mode==3?runtimeFailure:mode==4?errorFailure:null;
            if(expected!=null)require(failure==expected&&actual==sentinel);
            else if(mode==6||kind==1&&mode==7)require(failure instanceof ClassCastException&&actual==sentinel);
            else if(kind==1&&(mode==5||mode==8))require(failure instanceof NullPointerException&&actual==sentinel);
            else if(kind==1&&mode==9)require(failure==readFailure&&actual==sentinel);
            else if(kind==1&&mode==10)require(failure instanceof IndexOutOfBoundsException&&actual==sentinel);
            else require(failure==null&&actual==(kind==0?wanted:element));
            require(Trace.result==(mode==1?initial:wanted));
            if(actual==replacement)require(((Future)actual).tag==replacement.tag&&replacement.values==returnedValues);
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
