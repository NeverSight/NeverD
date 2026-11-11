import fixture.*;
import java.lang.reflect.*;
public final class PrivateInterfaceSeedResultHarness {
    static int mode,attempt,lengthCalls,subCalls,lastBegin,lastEnd;
    static Object returned;
    static Throwable failure;
    static void require(boolean value){if(!value)throw new AssertionError("owned private interface seed result");}
    @SuppressWarnings("unchecked") static <E extends Throwable> void raise(Throwable error)throws E{throw (E)error;}
    static final class Controlled extends Text {
        final boolean output;
        Controlled(boolean output){this.output=output;}
        public int length(){require(!output);lengthCalls++;if(mode==4)PrivateInterfaceSeedResultHarness.<RuntimeException>raise(failure);return mode==9?0:mode==8?2:4;}
        public char charAt(int index){throw new AssertionError("unexpected text character read");}
        public CharSequence subSequence(int begin,int end){require(!output);subCalls++;lastBegin=begin;lastEnd=end;if(mode==5||mode==6)PrivateInterfaceSeedResultHarness.<RuntimeException>raise(failure);if(mode==13)throw (RuntimeException)null;return (CharSequence)returned;}
        public String toString(){throw new AssertionError("unexpected text conversion");}
        public int hashCode(){throw new AssertionError("unexpected text hash");}
    }
    static Method metadata()throws Exception{
        Method m=Core.class.getDeclaredMethod("select",CharSequence.class,int.class);require(Modifier.isPrivate(m.getModifiers())&&!Modifier.isStatic(m.getModifiers())&&!m.isBridge()&&!m.isSynthetic());TypeVariable<Method>[] vars=m.getTypeParameters();require(vars.length==1&&vars[0].getGenericDeclaration().equals(m)&&vars[0].getBounds().length==1&&vars[0].getBounds()[0]==CharSequence.class&&CharSequence.class.isInterface());require(m.getGenericParameterTypes()[0].equals(vars[0])&&m.getGenericParameterTypes()[1]==int.class&&m.getGenericReturnType().equals(vars[0])&&m.getReturnType()==CharSequence.class&&Core.class.getTypeParameters().length==0);m.setAccessible(true);return m;
    }
    public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);Method helper=metadata();Throwable previous=null;
        for(attempt=0;attempt<2;attempt++){
            lengthCalls=subCalls=lastBegin=lastEnd=0;failure=attempt==1?new Exception("owned exact checked"):mode==6?new AssertionError("owned exact error"):new IllegalArgumentException("owned exact runtime");Core outer=new Core(),first=new Core(),second=new Core();Core target=attempt==0?first:second;if(mode==11)target=null;
            CharSequence value=mode==3||mode==14?null:mode==12&&route!=2?"abcd":new Controlled(false);int size=mode==7||mode==14||mode==11?(attempt==0?0:Integer.MIN_VALUE):mode==10?Integer.MAX_VALUE:2;returned=mode==2?null:mode==1||mode==12&&route==2?new Controlled(true):new String("xy");Object result=null;Throwable error=null;
            try{if(route==0)result=target.send(value,size);else if(route==1)result=outer.peer(target,value,size);else if(route==2)result=target.typed((Text)value,size);else try{result=helper.invoke(target,value,size);}catch(InvocationTargetException e){throw e.getCause();}}catch(Throwable e){error=e;}
            boolean nullReceiver=target==null,badSize=size<=0,plain=value instanceof String,shortValue=mode==8||mode==9||mode==10,early=mode==4,subFailure=mode==5||mode==6||mode==13,used=!nullReceiver&&!badSize&&value!=null&&!shortValue&&!early;
            require(outer.calls==0&&first.calls==(target==first?1:0)&&second.calls==(target==second?1:0));require(lengthCalls==(!nullReceiver&&!badSize&&value!=null&&!plain?1:0));require(subCalls==(used&&!plain?1:0));if(subCalls!=0)require(lastBegin==0&&lastEnd==size);
            Object expected=value==null?null:shortValue?value:plain?"ab":returned;boolean bridge=route==2&&used&&!subFailure&&returned!=null&&!(returned instanceof Text);
            if(nullReceiver||mode==2&&used||mode==13){require(error!=null&&error.getClass()==NullPointerException.class&&error!=previous&&result==null);}
            else if(badSize){require(error!=null&&error.getClass()==IllegalArgumentException.class&&error!=previous&&result==null);}
            else if(early||mode==5||mode==6)require(error==failure&&result==null);
            else if(bridge)require(error!=null&&error.getClass()==ClassCastException.class&&result==null);
            else{require(error==null);if(plain)require(result instanceof String&&result.equals(expected));else require(result==expected);}
            previous=error;
        }
        System.out.println("attempts=2,private-interface-seed-result=matched");
    }
}
