import fixture.Core;
import fixture.Probe;
import fixture.Result;
import fixture.Trace;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.util.Arrays;

public final class ConditionalArrayArgumentsHarness {
    private static void check(boolean value) {
        if (!value) throw new AssertionError("owned conditional array arguments mismatch");
    }
    private static Result make(int route, boolean flag, int size, Object left, Object right) throws Throwable {
        if (route==0) return Core.make(flag,size,left,right);
        if (route==1) return ConditionalArrayArgumentsCaller.make(flag,size,left,right);
        if (route==2) {
            try { return (Result)Core.class.getMethod("make",boolean.class,int.class,Object.class,Object.class)
                    .invoke(null,flag,size,left,right); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Result)MethodHandles.publicLookup().findStatic(Core.class,"make",
            MethodType.methodType(Result.class,boolean.class,int.class,Object.class,Object.class))
            .invokeExact(flag,size,left,right);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),branch=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        for (String field:new String[]{"selected","values"}) {
            int flags=Result.class.getField(field).getModifiers();
            check(Modifier.isPublic(flags)&&Modifier.isFinal(flags)&&!Modifier.isStatic(flags));
        }
        check(Result.class.getField("selected").getType()==Object.class);
        check(Result.class.getField("values").getType()==Object[].class);
        check(Arrays.equals(Result.class.getConstructor(Object.class,Object[].class).getExceptionTypes(),new Class[]{IOException.class}));
        check(Arrays.equals(Core.class.getMethod("make",boolean.class,int.class,Object.class,Object.class).getExceptionTypes(),new Class[]{IOException.class}));
        Result previous=null;
        Object[] previousArray=null;
        for (int attempt=0;attempt<2;attempt++) {
            final String initial=attempt==0?"I":"";
            final boolean flag=branch==1;
            final int size=mode==1?0:mode==2?1:mode==3?-1:3;
            final Object left=mode==6?null:mode==4?Integer.valueOf(123):new String("owned-left-"+attempt);
            final Object right=mode==6?null:mode==5?Integer.valueOf(456):new String("owned-right-"+attempt);
            final Object selected=flag?left:right;
            final Object[] alternate={new Object(),new Object()};
            final IOException checked=new IOException("owned array checked");
            final RuntimeException runtime=new IllegalStateException("owned array runtime");
            final Error error=new Error("owned array error");
            final StringBuilder events=new StringBuilder();
            final Object[][] capturedArray={null};
            final Result[] capturedResult={null};
            final Probe[] replacement={null};
            class Observer implements Probe {
                final boolean replaced;
                Observer(boolean value) { replaced=value; }
                public void initialize() {
                    check(!replaced&&events.length()==0&&initial.equals("I"));
                    events.append('I');
                    if (mode==13) throw error;
                }
                public Object select(Object value) throws IOException {
                    check(!replaced&&events.toString().equals(initial)&&value==selected);
                    events.append('P');
                    if (mode==7) throw checked;
                    return value;
                }
                public void allocated(Object[] values) throws IOException {
                    check(!replaced&&events.toString().equals(initial+"P")&&capturedArray[0]==null);
                    check(values.getClass()==String[].class&&values.length==size);
                    for (Object element:values) check(element==null);
                    capturedArray[0]=values;events.append('A');
                    if (mode==8) throw checked;
                }
                public Object element(int index, Object value) throws IOException {
                    check(index==0||index==1);
                    check(events.toString().equals(initial+(index==0?"PA":"PAE0")));
                    check(replaced==(mode==12&&index==1));
                    check(value==(index==0?left:right));
                    if (index==1) check(capturedArray[0][0]==left);
                    events.append('E').append(index);
                    if (mode==9&&index==0) throw checked;
                    if (mode==14&&index==1) throw runtime;
                    if (mode==12&&index==0) Trace.probe=replacement[0];
                    return value;
                }
                public Object[] finish(Object[] values, Object first, Object second) throws IOException {
                    check(!replaced&&events.toString().equals(initial+"PAE0E1"));
                    check(values==capturedArray[0]&&first==left&&second==right);
                    check(values[0]==left&&values[1]==right&&values[2]==null);
                    events.append('F');
                    if (mode==10) throw checked;
                    return mode==15?alternate:values;
                }
                public void constructed(Result value) throws IOException {
                    check(replaced==(mode==12)&&events.toString().equals(initial+"PAE0E1F"));
                    check(value.selected==selected&&value.values==(mode==15?alternate:capturedArray[0]));
                    check(capturedResult[0]==null);capturedResult[0]=value;events.append('C');
                    if (mode==11) throw checked;
                }
            }
            replacement[0]=new Observer(true);Trace.probe=new Observer(false);
            Result result=null;Throwable failure=null;
            try { result=make(route,flag,size,left,right); }
            catch (Throwable caught) { failure=caught; }
            final String tail;
            if (mode==13) tail="";
            else if (mode==3||mode==7) tail="P";
            else if (mode==8) tail="PA";
            else if (mode==1||mode==4||mode==9) tail="PAE0";
            else if (mode==2||mode==5||mode==14) tail="PAE0E1";
            else if (mode==10) tail="PAE0E1F";
            else tail="PAE0E1FC";
            check(events.toString().equals(initial+tail));
            if (mode==13) check(attempt==0?failure==error:failure instanceof NoClassDefFoundError);
            else if (mode==1||mode==2) check(failure instanceof ArrayIndexOutOfBoundsException);
            else if (mode==3) check(failure instanceof NegativeArraySizeException);
            else if (mode==4||mode==5) check(failure instanceof ArrayStoreException);
            else if (mode==7||mode==8||mode==9||mode==10||mode==11) check(failure==checked);
            else if (mode==14) check(failure==runtime);
            else check(failure==null&&result!=null&&result==capturedResult[0]);
            if (failure!=null) check(result==null);
            check((capturedResult[0]!=null)==tail.endsWith("C"));
            check((capturedArray[0]!=null)==tail.contains("A"));
            if (capturedArray[0]!=null) {
                Object[] values=capturedArray[0];check(values!=previousArray&&values.length==size);
                boolean firstStored=!(mode==1||mode==4||mode==8||mode==9);
                boolean secondStored=firstStored&&!(mode==2||mode==5||mode==14);
                for (int i=0;i<values.length;i++) check(values[i]==(i==0&&firstStored?left:i==1&&secondStored?right:null));
                previousArray=values;
            }
            if (capturedResult[0]!=null) {
                Result value=capturedResult[0];check(value!=previous&&value.selected==selected);
                check(value.values==(mode==15?alternate:capturedArray[0]));previous=value;
            }
            if (mode==12) check(Trace.probe==replacement[0]);
        }
        System.out.println("attempts=2,conditional-array-arguments=matched");
    }
}
