# import ocs2_double_integrator_noros_example # works with python2.7, should be working with python3, too

# python 3 workaround
import sys
sys.path.append("/home/jcarius/catkin_ws/devel/lib/python3.6/dist-packages/ocs2_double_integrator_noros_example")
from DoubleIntegratorPyBindings3 import mpc_interface, scalar_array, state_vector_array, input_vector_array_t

import numpy as np

mpc = mpc_interface("mpc")


time = 0.0
x = np.ndarray([2, 1])
x[0] = 0.3
x[1] = 0.5

mpc.setObservation(time, x)

mpc.advanceMpc()

# instantiate c++ std arrays to store results
t_result = scalar_array()
x_result = state_vector_array()
u_result = input_vector_array_t()

mpc.getMpcSolution(t_result, x_result, u_result)

print("MPC solution has", t_result.__len__(), "time steps")
print("t\t\tx\t\tu")

for t,x,u in zip(t_result, x_result, u_result):
  print(t, "\t\t", x, "\t\t", u)
