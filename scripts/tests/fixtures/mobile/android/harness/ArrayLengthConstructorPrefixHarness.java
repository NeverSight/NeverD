import fixture.Core;
import fixture.Probe;
import fixture.Trace;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.util.Arrays;

public final class ArrayLengthConstructorPrefixHarness {
    private static void check(boolean value) {
        if (!value) throw new AssertionError("owned array length constructor mismatch");
    }
    private static Core create(int route,int kind,byte[] data,int start,int length) throws Throwable {
        if (route==0) return kind==0?new Core(data):new Core(data,start,length);
        if (route==1) return ArrayLengthConstructorPrefixCaller.create(data,start,length,kind);
        if (route==2) {
            try {
                return kind==0?Core.class.getConstructor(byte[].class).newInstance((Object)data)
                    :Core.class.getConstructor(byte[].class,int.class,int.class).newInstance(data,start,length);
            } catch (InvocationTargetException failure) {throw failure.getCause();}
        }
        if (kind==0) return (Core)MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class,byte[].class)).invokeExact(data);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class,byte[].class,int.class,int.class)).invokeExact(data,start,length);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        check(Core.class.getField("data").getType()==byte[].class);
        for (String field:new String[]{"data","start","length"}) {
            int flags=Core.class.getField(field).getModifiers();
            check(Modifier.isPublic(flags)&&Modifier.isFinal(flags)&&!Modifier.isStatic(flags));
        }
        check(Arrays.equals(Core.class.getConstructor(byte[].class).getExceptionTypes(),new Class[]{IOException.class}));
        check(Arrays.equals(Core.class.getConstructor(byte[].class,int.class,int.class).getExceptionTypes(),new Class[]{IOException.class}));
        Core previous=null;
        for (int attempt=0;attempt<2;attempt++) {
            final byte[] input=mode==2?null:mode==1?new byte[0]:new byte[]{4,-3,2,1};
            final int suppliedStart=Integer.MAX_VALUE-attempt*3,suppliedLength=-1234567+attempt;
            final int beginValue=Integer.MIN_VALUE+attempt*7;
            final int expectedStart=kind==0?beginValue:suppliedStart;
            final int expectedLength=kind==0?(input==null?-1:input.length):suppliedLength;
            final IOException checked=new IOException("owned constructor checked");
            final RuntimeException runtime=new IllegalStateException("owned constructor runtime");
            final Error error=new Error("owned constructor error");
            final StringBuilder events=new StringBuilder();
            final Core[] captured={null};
            final Probe[] replacement={null};
            class Observer implements Probe {
                final boolean replaced;
                Observer(boolean value) {replaced=value;}
                public int begin() {
                    check(kind==0&&!replaced&&events.length()==0&&captured[0]==null);
                    events.append('B');
                    if (input!=null&&input.length>0) input[0]++;
                    if (mode==3) throw runtime;
                    if (mode==6) Trace.probe=replacement[0];
                    return beginValue;
                }
                public void constructed(Core value) throws IOException {
                    check(events.toString().equals(kind==0?"B":"")&&captured[0]==null);
                    check(replaced==(kind==0&&mode==6));
                    check(value.data==input&&value.start==expectedStart&&value.length==expectedLength);
                    events.append('C');captured[0]=value;
                    if (mode==4) throw checked;
                    if (mode==8) throw runtime;
                }
                public void after(Core value) throws IOException {
                    check(kind==0&&events.toString().equals("BC")&&captured[0]==value);
                    check(replaced==(mode==6)&&value.data==input&&value.start==expectedStart&&value.length==expectedLength);
                    events.append('A');
                    if (mode==5) throw checked;
                    if (mode==9) throw error;
                }
            }
            replacement[0]=new Observer(true);
            Trace.probe=mode==7?null:new Observer(false);
            Core result=null;Throwable failure=null;
            try {result=create(route,kind,input,suppliedStart,suppliedLength);}
            catch (Throwable caught) {failure=caught;}
            boolean began=kind==0&&mode!=7;
            boolean constructed=mode!=7&&!(kind==0&&(mode==2||mode==3));
            boolean after=kind==0&&constructed&&mode!=4&&mode!=8;
            check(events.toString().equals((began?"B":"")+(constructed?"C":"")+(after?"A":"")));
            if (mode==7||kind==0&&mode==2) check(failure instanceof NullPointerException);
            else if (mode==4||kind==0&&mode==5) check(failure==checked);
            else if (mode==8||kind==0&&mode==3) check(failure==runtime);
            else if (kind==0&&mode==9) check(failure==error);
            else check(failure==null&&result!=null&&result==captured[0]);
            if (failure!=null) check(result==null);
            check((captured[0]!=null)==constructed);
            if (captured[0]!=null) {
                check(captured[0]!=previous&&captured[0].data==input
                    &&captured[0].start==expectedStart&&captured[0].length==expectedLength);
                previous=captured[0];
            }
            if (input!=null&&input.length>0) check(input[0]==(began?5:4));
            if (kind==0&&mode==6) check(Trace.probe==replacement[0]);
        }
        System.out.println("attempts=2,array-length-constructor=matched");
    }
}
